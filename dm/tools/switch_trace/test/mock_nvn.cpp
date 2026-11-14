// A mock NVN driver and platform for running nvn_trace.cpp on the Mac (ST_MOCK). It checks the module's logic, not a console:
//   * a "game" records two command buffers a frame on two threads (render, compute, mesh work, events, counters and copies in one; indirect compute in the other) and
//     presents; its programs come through every path the module covers: nvnProgramSetShadersExt, nvnProgramSetShaders, the NVN2 builder, and an NVN 1 control section;
//   * the "GPU" runs a submission two presents after it was submitted, advancing a clock by a known cost per command, writing the timestamps asked for and executing
//     texture-to-buffer copies, so every interval's time and every dump's bytes are known in advance;
//   * the capture starts from the button combo (held, then released) and must be written without any sync wait other than a zero-timeout poll.
// test/check.py checks the files it writes in $ST_SD (default ./sd).
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include <deque>
#include <map>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/platform.h"
#include "../src/sha256.h"

// ---------------------------------------------------------------- platform (ST_MOCK)
static std::string sd_path(const char *p) { const char *root = getenv("ST_SD") ? getenv("ST_SD") : "sd"; return std::string(root) + (strncmp(p, "sd:", 3) ? p : p + 3); }
static st_bootstrap_fn g_repl;
static void *mock_bootstrap(const char *name);
st_bootstrap_fn st_hook_bootstrap(st_bootstrap_fn r) { g_repl = r; return mock_bootstrap; }
int st_fs_mount(void) { return 0; }
int st_fs_exists(const char *p) { struct stat st; return stat(sd_path(p).c_str(), &st) == 0; }
int st_fs_read_small(const char *p, char *buf, size_t n) { FILE *f = fopen(sd_path(p).c_str(), "rb"); if (!f) return -1; size_t k = fread(buf, 1, n - 1, f); fclose(f); buf[k] = 0; return (int)k; }
int st_fs_append(const char *p, const void *d, size_t n) { FILE *f = fopen(sd_path(p).c_str(), "ab"); if (!f) return -1; fwrite(d, 1, n, f); fclose(f); return 0; }
int st_fs_delete(const char *p) { return unlink(sd_path(p).c_str()); }
static std::atomic<uint32_t> g_buttons{0};
uint32_t st_buttons(void) { return g_buttons.load(); }
uint64_t st_cpu_ticks(void) { return (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count(); }
uint64_t st_cpu_tick_hz(void) { return (uint64_t)std::chrono::steady_clock::period::den / std::chrono::steady_clock::period::num; }
void st_log(const char *fmt, ...) { va_list v; va_start(v, fmt); fprintf(stderr, "[log] "); vfprintf(stderr, fmt, v); fprintf(stderr, "\n"); va_end(v); }
extern "C" void switch_trace_main(void);

// ---------------------------------------------------------------- the mock driver
typedef uint64_t Addr;
struct Pool { void *mem; size_t size; int flags; };
struct PoolBuilder { void *mem; size_t size; int flags; };
struct ShaderData { Addr data; const void *control; };
struct ShaderDataExt { Addr data; const void *control; uint64_t ext; };
struct Program { int n; };
struct Tex { int w, h, fmt, bpp; uint8_t *pixels; };
struct CopyRegion { int x, y, z, w, h, d; };
enum Op { OP_STAMP, OP_WORK, OP_COPY_T2B };
struct Cmd { Op op; uint64_t arg; const Tex *tex; };
struct CB { std::vector<Cmd> cmds; };
struct Handle { std::vector<Cmd> cmds; };
struct Sync { std::atomic<int> signaled{0}; };
struct Queue {};
static const uint64_t C_DRAW = 10000, C_GROUP = 1000, C_MESH = 20000, C_INDIRECT = 50000, C_BARRIER = 500;   // ns of mock GPU time

static std::mutex g_gpu_mu;
static uint64_t g_gpu_clock = 1000000000ull;
struct Pending { int due; std::vector<Cmd> cmds; Sync *sync; };
static std::deque<Pending> g_pending;
static int g_presents;
static std::atomic<int> g_nonzero_waits{0};
static void gpu_run(int upto) {   // run every submission due by present `upto`, in order
    std::lock_guard<std::mutex> l(g_gpu_mu);
    while (!g_pending.empty() && g_pending.front().due <= upto) {
        Pending p = std::move(g_pending.front()); g_pending.pop_front();
        for (const Cmd &c : p.cmds) {
            if (c.op == OP_WORK) g_gpu_clock += c.arg;
            else if (c.op == OP_STAMP) { uint64_t *slot = (uint64_t *)(uintptr_t)c.arg; slot[0] = 0; slot[1] = g_gpu_clock; }
            else memcpy((void *)(uintptr_t)c.arg, c.tex->pixels, (size_t)c.tex->w * c.tex->h * c.tex->bpp);
        }
        if (p.sync) p.sync->signaled = 1;
    }
}
static CB *cbp(void *c) { return (CB *)c; }
#define F(ret, name, ...) static ret m_##name(__VA_ARGS__)
F(int, nvnDeviceInitialize, void *, const void *) { return 1; }
F(uint64_t, nvnDeviceGetTimestampInNanoseconds, const void *, const uint64_t *c) { return c[1]; }
F(void, nvnMemoryPoolBuilderSetDefaults, PoolBuilder *b) { memset(b, 0, sizeof *b); }
F(void, nvnMemoryPoolBuilderSetDevice, PoolBuilder *, void *) {}
F(void, nvnMemoryPoolBuilderSetStorage, PoolBuilder *b, void *m, size_t n) { b->mem = m; b->size = n; }
F(void, nvnMemoryPoolBuilderSetFlags, PoolBuilder *b, int f) { b->flags = f; }
F(int, nvnMemoryPoolInitialize, Pool *p, const PoolBuilder *b) { p->mem = b->mem; p->size = b->size; p->flags = b->flags; return 1; }
F(void, nvnMemoryPoolFinalize, Pool *) {}
F(void *, nvnMemoryPoolMap, const Pool *p) { return p->mem; }
F(Addr, nvnMemoryPoolGetBufferAddress, const Pool *p) { return (Addr)(uintptr_t)p->mem; }   // GPU address = CPU address in the mock
F(size_t, nvnMemoryPoolGetSize, const Pool *p) { return p->size; }
F(int, nvnMemoryPoolGetFlags, const Pool *p) { return p->flags; }
F(int, nvnProgramSetShaders, Program *p, int n, const ShaderData *) { p->n = n; return 1; }
F(int, nvnProgramSetShadersExt, Program *p, int n, const ShaderDataExt *) { p->n = n; return 1; }
struct PB { int n; const ShaderData *sd; };
F(void, nvnProgramBuilderSetShaders, PB *b, int n, const ShaderData *sd) { b->n = n; b->sd = sd; }
F(int, nvnProgramInitializeFromBuilder, Program *p, const PB *b) { p->n = b->n; return 1; }
F(int, nvnTextureGetWidth, const Tex *t) { return t->w; }
F(int, nvnTextureGetHeight, const Tex *t) { return t->h; }
F(int, nvnTextureGetFormat, const Tex *t) { return t->fmt; }
F(int, nvnSyncInitialize, Sync *s, void *) { new (s) Sync(); return 1; }
F(int, nvnSyncWait, const Sync *s, uint64_t timeout) { if (timeout) g_nonzero_waits++; return ((Sync *)s)->signaled ? 0 : 2; }
F(void, nvnQueueFenceSync, Queue *, Sync *s, int, int) { std::lock_guard<std::mutex> l(g_gpu_mu); s->signaled = 0; g_pending.push_back({g_presents + 2, {}, s}); }
F(int, nvnQueueWaitSync, Queue *, const Sync *) { return 1; }
F(void, nvnQueueSubmitCommands, Queue *, int n, const uint64_t *h) { std::lock_guard<std::mutex> l(g_gpu_mu); for (int i = 0; i < n; i++) g_pending.push_back({g_presents + 2, ((Handle *)(uintptr_t)h[i])->cmds, nullptr}); }
F(void, nvnQueuePresentTexture, Queue *, void *, int) { g_presents++; gpu_run(g_presents); }
F(void, nvnCommandBufferBeginRecording, void *c) { cbp(c)->cmds.clear(); }
F(uint64_t, nvnCommandBufferEndRecording, void *c) { Handle *h = new Handle{cbp(c)->cmds}; return (uint64_t)(uintptr_t)h; }
F(void, nvnCommandBufferReportCounter, void *c, int type, Addr a) { if (type == 0) cbp(c)->cmds.push_back({OP_STAMP, a, nullptr}); }
F(void, nvnCommandBufferSetRenderTargets, void *, int, const void *const *, const void *const *, const void *, const void *) {}
F(void, nvnCommandBufferBindProgram, void *, const void *, int) {}
F(void, nvnCommandBufferBarrier, void *c, int) { cbp(c)->cmds.push_back({OP_WORK, C_BARRIER, nullptr}); }
F(void, nvnCommandBufferDispatchCompute, void *c, int x, int y, int z) { cbp(c)->cmds.push_back({OP_WORK, C_GROUP * (uint64_t)x * y * z, nullptr}); }
F(void, nvnCommandBufferDispatchComputeIndirect, void *c, Addr) { cbp(c)->cmds.push_back({OP_WORK, C_INDIRECT, nullptr}); }
F(void, nvnCommandBufferDrawMeshTasks, void *c, int, int) { cbp(c)->cmds.push_back({OP_WORK, C_MESH, nullptr}); }
F(void, nvnCommandBufferDrawMeshTasksIndirect, void *c, Addr) { cbp(c)->cmds.push_back({OP_WORK, C_MESH, nullptr}); }
F(void, nvnCommandBufferDrawElements, void *c, int, int, int, Addr) { cbp(c)->cmds.push_back({OP_WORK, C_DRAW, nullptr}); }
F(void, nvnCommandBufferDrawArrays, void *c, int, int, int) { cbp(c)->cmds.push_back({OP_WORK, C_DRAW, nullptr}); }
F(void, nvnCommandBufferSignalEvent, void *, const void *, int, int, int, uint32_t) {}
F(void, nvnCommandBufferWaitEvent, void *, const void *, int, uint32_t) {}
F(void, nvnCommandBufferWaitSync, void *, const void *) {}
F(void, nvnCommandBufferFenceSync, void *, void *, int, int) {}
F(void, nvnCommandBufferSetRenderEnable, void *, uint32_t) {}
F(void, nvnCommandBufferSetRenderEnableConditional, void *, int, Addr) {}
F(void, nvnCommandBufferClearColor, void *, int, const float *, int) {}
F(void, nvnCommandBufferClearDepthStencil, void *, float, uint32_t, int, int) {}
F(void, nvnCommandBufferCopyBufferToBuffer, void *, Addr, Addr, size_t, int) {}
F(void, nvnCommandBufferCopyTextureToBuffer, void *c, const Tex *t, const void *, const CopyRegion *r, Addr dst, int) {
    if (r->w != t->w || r->h != t->h) { fprintf(stderr, "[mock] copy region %dx%d differs from the texture %dx%d\n", r->w, r->h, t->w, t->h); abort(); }
    cbp(c)->cmds.push_back({OP_COPY_T2B, dst, t});
}
static std::atomic<int> g_row_stride_calls{0};
F(void, nvnCommandBufferSetCopyRowStride, void *, ptrdiff_t) { g_row_stride_calls++; }
F(void, nvnCommandBufferSetCopyImageStride, void *, ptrdiff_t) {}
static void *m_gpa(const void *, const char *name);
static std::map<std::string, void *> g_procs = {
#define P(n) {#n, (void *)m_##n}
    P(nvnDeviceInitialize), P(nvnDeviceGetTimestampInNanoseconds), P(nvnMemoryPoolBuilderSetDefaults), P(nvnMemoryPoolBuilderSetDevice), P(nvnMemoryPoolBuilderSetStorage),
    P(nvnMemoryPoolBuilderSetFlags), P(nvnMemoryPoolInitialize), P(nvnMemoryPoolFinalize), P(nvnMemoryPoolMap), P(nvnMemoryPoolGetBufferAddress), P(nvnMemoryPoolGetSize),
    P(nvnMemoryPoolGetFlags), P(nvnProgramSetShaders), P(nvnProgramSetShadersExt), P(nvnProgramBuilderSetShaders), P(nvnProgramInitializeFromBuilder),
    P(nvnTextureGetWidth), P(nvnTextureGetHeight), P(nvnTextureGetFormat), P(nvnSyncInitialize), P(nvnSyncWait), P(nvnQueueFenceSync), P(nvnQueueWaitSync),
    P(nvnQueueSubmitCommands), P(nvnQueuePresentTexture), P(nvnCommandBufferBeginRecording), P(nvnCommandBufferEndRecording), P(nvnCommandBufferReportCounter),
    P(nvnCommandBufferSetRenderTargets), P(nvnCommandBufferBindProgram), P(nvnCommandBufferBarrier), P(nvnCommandBufferDispatchCompute),
    P(nvnCommandBufferDispatchComputeIndirect), P(nvnCommandBufferDrawMeshTasks), P(nvnCommandBufferDrawMeshTasksIndirect), P(nvnCommandBufferDrawElements),
    P(nvnCommandBufferDrawArrays), P(nvnCommandBufferSignalEvent), P(nvnCommandBufferWaitEvent), P(nvnCommandBufferWaitSync), P(nvnCommandBufferFenceSync),
    P(nvnCommandBufferSetRenderEnable), P(nvnCommandBufferSetRenderEnableConditional), P(nvnCommandBufferClearColor), P(nvnCommandBufferClearDepthStencil),
    P(nvnCommandBufferCopyBufferToBuffer), P(nvnCommandBufferCopyTextureToBuffer), P(nvnCommandBufferSetCopyRowStride), P(nvnCommandBufferSetCopyImageStride),
#undef P
};
static void *m_gpa(const void *, const char *name) { auto it = g_procs.find(name); return it == g_procs.end() ? nullptr : it->second; }
static void *mock_bootstrap(const char *name) { return !strcmp(name, "nvnDeviceGetProcAddress") ? (void *)m_gpa : m_gpa(nullptr, name); }

// ---------------------------------------------------------------- the "game"
typedef void *(*GPA)(const void *, const char *);
#define G(name, ret, ...) static ret (*name)(__VA_ARGS__)
G(DevInit, int, void *, const void *); G(PoolBSetDefaults, void, PoolBuilder *); G(PoolBSetStorage, void, PoolBuilder *, void *, size_t); G(PoolBSetFlags, void, PoolBuilder *, int);
G(PoolInit, int, Pool *, const PoolBuilder *); G(SetShaders, int, Program *, int, const ShaderData *); G(SetShadersExt, int, Program *, int, const ShaderDataExt *);
G(PBSetShaders, void, PB *, int, const ShaderData *); G(InitFromBuilder, int, Program *, const PB *);
G(Begin, void, void *); G(End, uint64_t, void *); G(SetRT, void, void *, int, const void *const *, const void *const *, const void *, const void *); G(Bind, void, void *, const void *, int);
G(Barrier, void, void *, int); G(Dispatch, void, void *, int, int, int); G(DispatchInd, void, void *, Addr); G(Mesh, void, void *, int, int); G(MeshInd, void, void *, Addr);
G(DrawEl, void, void *, int, int, int, Addr); G(Submit, void, Queue *, int, const uint64_t *); G(Present, void, Queue *, void *, int); G(RepCtr, void, void *, int, Addr);
G(WaitEv, void, void *, const void *, int, uint32_t); G(SignalEv, void, void *, const void *, int, int, int, uint32_t); G(RenderEnable, void, void *, uint32_t);
G(RenderCond, void, void *, int, Addr); G(ClearColor, void, void *, int, const float *, int); G(QFence, void, Queue *, Sync *, int, int); G(RowStride, void, void *, ptrdiff_t);

static uint8_t g_shader_mem[1 << 16] __attribute__((aligned(4096)));
static void make_nvn2(uint8_t *ctl, uint32_t code_off, uint32_t ncode, uint8_t seed) {   // an NVN2 control section and its code
    memset(ctl, 0, 0x100); uint32_t magic = 0x98761234u, ver = 0xc9; memcpy(ctl, &magic, 4); memcpy(ctl + 4, &ver, 4);
    ctl[8] = seed; ctl[0x30] = 0xAB;   // +0x30..0x33 is zeroed by the key rule
    memcpy(ctl + 0x44, &ncode, 4);
    for (uint32_t i = 0; i < ncode; i++) g_shader_mem[code_off + i] = (uint8_t)(seed * 31 + i * 7);
}
static void make_nvn1(uint8_t *ctl, uint32_t code_off, uint32_t ncode, uint8_t seed) {   // an NVN (Switch 1) control section: 0x880 key bytes, code length at +0x704
    for (int i = 0; i < 0x880; i++) ctl[i] = (uint8_t)(seed + i * 13);
    uint32_t own = 0; memcpy(ctl + 0x20, &own, 4); memcpy(ctl + 0x704, &ncode, 4);
    for (uint32_t i = 0; i < ncode; i++) g_shader_mem[code_off + i] = (uint8_t)(seed * 31 + i * 7);
}

int main(int argc, char **argv) {
    int frames = argc > 1 ? atoi(argv[1]) : 200;
    switch_trace_main();
    GPA gpa = (GPA)g_repl("nvnDeviceGetProcAddress");
#define R(var, name) var = (decltype(var))gpa(nullptr, #name)
    R(DevInit, nvnDeviceInitialize); R(PoolBSetDefaults, nvnMemoryPoolBuilderSetDefaults); R(PoolBSetStorage, nvnMemoryPoolBuilderSetStorage); R(PoolBSetFlags, nvnMemoryPoolBuilderSetFlags);
    R(PoolInit, nvnMemoryPoolInitialize); R(SetShaders, nvnProgramSetShaders); R(SetShadersExt, nvnProgramSetShadersExt); R(PBSetShaders, nvnProgramBuilderSetShaders);
    R(InitFromBuilder, nvnProgramInitializeFromBuilder); R(Begin, nvnCommandBufferBeginRecording); R(End, nvnCommandBufferEndRecording); R(SetRT, nvnCommandBufferSetRenderTargets);
    R(Bind, nvnCommandBufferBindProgram); R(Barrier, nvnCommandBufferBarrier); R(Dispatch, nvnCommandBufferDispatchCompute); R(DispatchInd, nvnCommandBufferDispatchComputeIndirect);
    R(Mesh, nvnCommandBufferDrawMeshTasks); R(MeshInd, nvnCommandBufferDrawMeshTasksIndirect); R(DrawEl, nvnCommandBufferDrawElements); R(Submit, nvnQueueSubmitCommands);
    R(Present, nvnQueuePresentTexture); R(RepCtr, nvnCommandBufferReportCounter); R(WaitEv, nvnCommandBufferWaitEvent); R(SignalEv, nvnCommandBufferSignalEvent);
    R(RenderEnable, nvnCommandBufferSetRenderEnable); R(RenderCond, nvnCommandBufferSetRenderEnableConditional); R(ClearColor, nvnCommandBufferClearColor);
    R(QFence, nvnQueueFenceSync); R(RowStride, nvnCommandBufferSetCopyRowStride);
    char dev[64]; DevInit(dev, nullptr);
    Pool sp; PoolBuilder pb; PoolBSetDefaults(&pb); PoolBSetStorage(&pb, g_shader_mem, sizeof g_shader_mem); PoolBSetFlags(&pb, 0x2 | 0x20 | 0x40); PoolInit(&sp, &pb);
    Addr base = (Addr)(uintptr_t)g_shader_mem;
    static uint8_t ctl[5][0x100], ctl1[0x880];
    make_nvn2(ctl[0], 0x0000, 0x800, 1); make_nvn2(ctl[1], 0x1000, 0x900, 2);   // vs, fs: through SetShadersExt
    make_nvn2(ctl[2], 0x2000, 0x700, 3);                                       // compute: through SetShaders
    make_nvn2(ctl[3], 0x3000, 0x600, 4); make_nvn2(ctl[4], 0x4000, 0x500, 5);   // task, mesh: through the builder
    make_nvn1(ctl1, 0x5000, 0x400, 6);                                         // an NVN (Switch 1) compute shader through SetShaders, with key_skip
    Program pg, pc, pm, p1;
    { ShaderDataExt sd[2] = {{base + 0x0000, ctl[0], 7}, {base + 0x1000, ctl[1], 0}}; SetShadersExt(&pg, 2, sd); }
    { ShaderData sd[1] = {{base + 0x2000, ctl[2]}}; SetShaders(&pc, 1, sd); }
    { ShaderData sd[2] = {{base + 0x3000, ctl[3]}, {base + 0x4000, ctl[4]}}; PB b; PBSetShaders(&b, 2, sd); InitFromBuilder(&pm, &b); }
    { ShaderData sd[1] = {{base + 0x5000, ctl1}}; SetShaders(&p1, 1, sd); memset(ctl1, 0xEE, sizeof ctl1); }   // the control section is only valid during the call
    // textures with known contents: RGBA8 1440x810 (bpp 4), its depth (DEPTH32F), RGBA16F 720x405 (bpp 8)
    auto make_tex = [](int w, int h, int fmt, int bpp, uint8_t seed) { Tex t = {w, h, fmt, bpp, (uint8_t *)malloc((size_t)w * h * bpp)}; for (size_t i = 0; i < (size_t)w * h * bpp; i++) t.pixels[i] = (uint8_t)(seed + i * 3); return t; };
    Tex color = make_tex(1440, 810, 0x25, 4, 11), depth = make_tex(1440, 810, 0x34, 4, 22), half = make_tex(720, 405, 0x29, 8, 33);
    { FILE *f = fopen(sd_path("sd:/tex_color.bin").c_str(), "wb"); fwrite(color.pixels, 1, (size_t)1440 * 810 * 4, f); fclose(f);
      f = fopen(sd_path("sd:/tex_half.bin").c_str(), "wb"); fwrite(half.pixels, 1, (size_t)720 * 405 * 8, f); fclose(f); }
    const void *c1[1] = {&color}, *c2[1] = {&half};
    char event_a[8], event_b[8]; Sync qsync; new (&qsync) Sync();
    float black[4] = {0, 0, 0, 0};
    Queue q; CB cbA, cbB;
    for (int f = 0; f < frames; f++) {
        if (f == 10) g_buttons = 1u << 4 | 1u << 5;   // LS + RS held
        if (f == 12) g_buttons = 0;                   // released: the capture's first frame is 12 + 1 + delay
        uint64_t hA = 0, hB = 0;
        std::thread ta([&] {   // render thread: R (5 draws) / C run of 3 / R (1 draw) / M run of 2 / R half-size (barrier, 2 draws)
            Begin(&cbA); RowStride(&cbA, 4096); SetRT(&cbA, 1, c1, nullptr, &depth, nullptr); ClearColor(&cbA, 0, black, 0xf); Bind(&cbA, &pg, 0x3);
            for (int i = 0; i < 5; i++) DrawEl(&cbA, 4, 2, 36, 0);
            Bind(&cbA, &pc, 0x20); Dispatch(&cbA, 4, 2, 1); Dispatch(&cbA, 8, 1, 1); Dispatch(&cbA, 2, 2, 2);
            DrawEl(&cbA, 4, 2, 36, 0);
            Bind(&cbA, &pm, 0x40 | 0x80 | 0x2); Mesh(&cbA, 0, 16); MeshInd(&cbA, 0);
            WaitEv(&cbA, event_a, 1, 7);
            SetRT(&cbA, 1, c2, nullptr, nullptr, nullptr); Bind(&cbA, &pg, 0x3); Barrier(&cbA, 1);
            RenderCond(&cbA, 1, 0x7000); DrawEl(&cbA, 4, 2, 36, 0); DrawEl(&cbA, 4, 2, 36, 0); RenderEnable(&cbA, 1);
            for (int i = 0; i < 3; i++) RepCtr(&cbA, 1, 0x5000 + 16 * i);   // the game's own reports: occlusion queries and a frame timer
            RepCtr(&cbA, 0x10, 0x9000);
            SignalEv(&cbA, event_b, 0, 1, 0, 9);
            hA = End(&cbA);
        });
        std::thread tb([&] {   // compute thread: an NVN 1 program, two indirect dispatches
            Begin(&cbB); Bind(&cbB, &p1, 0x20); DispatchInd(&cbB, 0); DispatchInd(&cbB, 0); hB = End(&cbB);
        });
        ta.join(); tb.join();
        if (f == 30) {   // the per-call cost of a wrapped draw while a capture is armed (a command buffer never submitted: no rows)
            CB cbC; auto t0 = std::chrono::steady_clock::now();
            Begin(&cbC); SetRT(&cbC, 1, c1, nullptr, &depth, nullptr); Bind(&cbC, &pg, 0x3);
            const int N = 50000; for (int i = 0; i < N; i++) { DrawEl(&cbC, 4, 2, 36, 0); if ((i & 1023) == 1023) cbC.cmds.clear(); }
            End(&cbC);
            fprintf(stderr, "[bench] armed wrapped DrawElements: %.1f ns a call (mock driver included)\n", std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / N);
        }
        uint64_t hs[2] = {hB, hA}; Submit(&q, 2, hs);
        QFence(&q, &qsync, 0, 0);
        Present(&q, nullptr, 0);
    }
    {   // idle per-call cost, against the same mock call through a pointer
        auto t0 = std::chrono::steady_clock::now();
        Begin(&cbA); SetRT(&cbA, 1, c1, nullptr, &depth, nullptr); Bind(&cbA, &pg, 0x3);
        const int N = 2000000; for (int i = 0; i < N; i++) { DrawEl(&cbA, 4, 2, 36, 0); if ((i & 1023) == 1023) cbA.cmds.clear(); }
        End(&cbA);
        double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / N;
        fprintf(stderr, "[bench] idle wrapped DrawElements: %.1f ns a call (mock driver included)\n", ns);
        void (*volatile real_draw)(void *, int, int, int, Addr) = m_nvnCommandBufferDrawElements;
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; i++) { real_draw(&cbA, 4, 2, 36, 0); if ((i & 1023) == 1023) cbA.cmds.clear(); }
        fprintf(stderr, "[bench] unwrapped mock DrawElements (through a pointer): %.1f ns a call\n", std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / N);
    }
    fprintf(stderr, "[mock] presents %d, sync waits with a non-zero timeout: %d\n", g_presents, g_nonzero_waits.load());
    return g_nonzero_waits.load() ? 1 : 0;
}
