// switch_trace: console ground truth for nxnative, recorded inside a game on a Switch or Switch 2 by a code-injection module. NOT YET RUN ON A CONSOLE: the logic
// runs on the Mac against a mock NVN driver (../test/run.sh). See ../README.md.
//
// Three capture modes (config mode=timing,seq,dump; any combination), each a short burst of frames started by a trigger:
//   timing  GPU time per interval, the way the host's DM_NVN_GPUPROF times encoders: a timestamp at each SetRenderTargets, around each run of dispatches and of mesh draws
//           (split=run) or each one (split=each), and where command buffers begin and end. Shader keys of the programs each interval used.
//   seq     the ordered command stream: render targets, binds (with keys), draws, dispatches, mesh draws, barriers, event waits / signals, syncs, counter reports,
//           render enable / conditional render, clears and copies, per command buffer in submission order, with the queue's submits, waits, fences and presents.
//           No GPU work is added.
//   dump    render targets (and optional buffer ranges) copied after the intervals that used a named shader key (dump_keys=) or after every render interval
//           (dump_all=1), for reference images.
//
// How it hooks: the game resolves NVN through nvnBootstrapLoader / nvnDeviceGetProcAddress. The module replaces nvnBootstrapLoader (platform.h st_hook_bootstrap) and
// hands out its own wrappers for the functions below; every other name passes through. Idle (no capture), each wrapper is one relaxed load of a flag and the real call.
//
// Shader keys are nxnative's (dm/gfx/nvn/hle_nvn.c keys_hash), the same 16-hex keys as a game's shader_map.tsv and the host's [pass] lines:
//   NVN2 (Switch 2; control magic 0x98761234, version 0xc9): SHA-256 of the control section's first 0x48 bytes with +0x30..+0x33 as zero, then the code (length at +0x44).
//   NVN (Switch 1): SHA-256 of the control section's first 0x880 bytes (or its own length at +0x20 when that is 0x760..0x87f), with the profile's shader_key_skip range
//   as zeros (config key_skip=OFF:LEN), then the code (length at +0x704).
// The first 8 bytes of the digest, printed big-endian. Programs come from nvnProgramSetShaders, nvnProgramSetShadersExt (stage stride: config ext_stride, 0x18 by
// default) and NVN2's nvnProgramBuilderSetShaders + nvnProgramInitializeFromBuilder. NVN2 stages keep a copy of their 0x48 key bytes and are hashed after the capture,
// only if it used them (a Switch 2 game makes thousands of programs, with their code in CPU-uncached pools); NVN stages are hashed when the program is made.
//
// Readback is asynchronous: after the last logged frame the module fences each queue the capture used (no flush, no wait: the game's next present flushes the fences)
// and polls them with a zero timeout at each later present; when all have signalled it writes the capture's files in one go. Logged frames never wait on the GPU and
// never touch the SD card.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <strings.h>
#include <atomic>
#include "platform.h"
#include "sha256.h"

// ---------------------------------------------------------------- NVN, as far as the module needs it (opaque objects)
typedef uint64_t NVNbufferAddress; typedef uint64_t NVNcommandHandle; typedef int NVNboolean;
struct NVNdevice; struct NVNqueue; struct NVNcommandBuffer; struct NVNtexture; struct NVNtextureView; struct NVNprogram; struct NVNwindow; struct NVNevent; struct NVNdeviceBuilder;
// The module's own objects. NVN2's sizes are in no public header, so these are oversized (NVN only writes within its own size). NVN 1: pool 0x100, builder 0x40, sync 0x40
struct alignas(16) NVNmemoryPool { char reserved[0x400]; };
struct alignas(16) NVNmemoryPoolBuilder { char reserved[0x400]; };
struct alignas(16) NVNsync { char reserved[0x400]; };
struct NVNshaderData { NVNbufferAddress data; const void *control; };
struct NVNcounterData { uint64_t counter, timestamp; };
struct NVNcopyRegion { int xoffset, yoffset, zoffset, width, height, depth; };
enum { NVN_COUNTER_TYPE_TIMESTAMP = 0, NVN_COUNTER_TYPE_TIMESTAMP_TOP = 0x10 };
enum { NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED = 0x2, NVN_MEMORY_POOL_FLAGS_CPU_CACHED = 0x4, NVN_MEMORY_POOL_FLAGS_GPU_CACHED = 0x20 };
enum { NVN_SHADER_STAGE_COMPUTE_BIT = 0x20 };
enum { NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE = 0 };
enum { NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED = 0, NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED = 1 };
enum { NVN_BARRIER_ORDER_FRAGMENTS_BIT = 0x2, NVN_BARRIER_INVALIDATE_TEXTURE_BIT = 0x10 };
typedef void (*PFN)(void);

// Bytes per pixel of each NVNformat (0: block-compressed or none: not dumped), from dm/gfx/nvn/include/nvn.h
static const uint8_t FORMAT_BPP[] = {
    0,1,1,1,1,2,2,2,2,2,4,4,4,2,2,2,2,4,4,4,4,4,8,8,8,3,3,3,3,6,6,6,6,6,12,12,12,4,4,4,4,8,8,8,8,8,16,16,16,1,2,4,4,4,8,4,4,2,2,2,2,4,4,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,1,1,2,2,4,4,2,2,4,4,8,8,3,3,6,6,12,12,4,4,8,8,16,16,4,4,4,4,4,4,4,4,8,8,8,8,8,16,16,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,4,4,4,4};
static int format_bpp(int f) { return f > 0 && f < (int)sizeof FORMAT_BPP ? FORMAT_BPP[f] : 0; }

// ---------------------------------------------------------------- configuration (<dir>/config.txt, key=value lines; README "Configuration")
enum { B_A, B_B, B_X, B_Y, B_LS, B_RS, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS, B_LEFT, B_UP, B_RIGHT, B_DOWN };   // nn::hid::NpadButton bit numbers
static const char *const BUTTON_NAMES[16] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "Plus", "Minus", "Left", "Up", "Right", "Down"};
enum { M_TIMING = 1, M_SEQ = 2, M_DUMP = 4 };
enum { MAXDUMPKEY = 16, MAXDUMPBUF = 4 };
static struct {
    int modes = M_TIMING;
    long start_frame = -1;              // first logged frame (presents counted from boot); -1: wait for the combo or the trigger file
    long frames = 120;                  // frames logged per capture
    long delay = 30;                    // frames from releasing the combo (or finding the go file) to the first logged frame
    uint32_t combo = 1u << B_LS | 1u << B_RS;   // held together, then released, starts a capture (0: off)
    int each = 0;                       // split=each: timing intervals per dispatch and mesh draw (split=run: per run of them)
    double gpu_ts_hz = 0;               // GPU timestamp rate, used only when nvnDeviceGetTimestampInNanoseconds is unavailable (0: raw ticks)
    uint32_t key_skip_off = 0, key_skip_len = 0;   // NVN 1 only: the game profile's shader_key_skip (bytes of the control section hashed as zeros)
    uint32_t ext_stride = 0x18;         // nvnProgramSetShadersExt's bytes per stage
    uint64_t dump_keys[MAXDUMPKEY]; int ndump_keys = 0;
    int dump_all = 0, dump_depth = 0, dump_max = 64;
    uint16_t dump_w = 0, dump_h = 0;    // dump only targets of this size (0: any)
    long dump_mb = 256;
    struct { NVNbufferAddress addr; uint32_t size; } dump_buf[MAXDUMPBUF]; int ndump_buf = 0;
} g_cfg;
#ifdef ST_TITLE_ID
static const char *DIR = "sd:/switch-trace/" ST_TITLE_ID;   // one folder per game when the module is built for one
#else
static const char *DIR = "sd:/switch-trace";
#endif

// ---------------------------------------------------------------- real entry points
static st_bootstrap_fn real_bootstrap;
static PFN (*real_gpa)(const NVNdevice *, const char *);
#define REAL(name, ret, ...) static ret (*real_##name)(__VA_ARGS__)
REAL(nvnDeviceInitialize, NVNboolean, NVNdevice *, const NVNdeviceBuilder *);
REAL(nvnDeviceGetTimestampInNanoseconds, uint64_t, const NVNdevice *, const NVNcounterData *);
REAL(nvnMemoryPoolBuilderSetDefaults, void, NVNmemoryPoolBuilder *);
REAL(nvnMemoryPoolBuilderSetDevice, void, NVNmemoryPoolBuilder *, NVNdevice *);
REAL(nvnMemoryPoolBuilderSetStorage, void, NVNmemoryPoolBuilder *, void *, size_t);
REAL(nvnMemoryPoolBuilderSetFlags, void, NVNmemoryPoolBuilder *, int);
REAL(nvnMemoryPoolInitialize, NVNboolean, NVNmemoryPool *, const NVNmemoryPoolBuilder *);
REAL(nvnMemoryPoolFinalize, void, NVNmemoryPool *);
REAL(nvnMemoryPoolMap, void *, const NVNmemoryPool *);
REAL(nvnMemoryPoolGetBufferAddress, NVNbufferAddress, const NVNmemoryPool *);
REAL(nvnMemoryPoolGetSize, size_t, const NVNmemoryPool *);
REAL(nvnMemoryPoolGetFlags, int, const NVNmemoryPool *);
REAL(nvnProgramSetShaders, NVNboolean, NVNprogram *, int, const NVNshaderData *);
REAL(nvnProgramSetShadersExt, NVNboolean, NVNprogram *, int, const void *);
REAL(nvnProgramBuilderSetShaders, void, void *, int, const NVNshaderData *);
REAL(nvnProgramInitializeFromBuilder, NVNboolean, NVNprogram *, const void *);
REAL(nvnTextureGetWidth, int, const NVNtexture *);
REAL(nvnTextureGetHeight, int, const NVNtexture *);
REAL(nvnTextureGetFormat, int, const NVNtexture *);
REAL(nvnSyncInitialize, NVNboolean, NVNsync *, NVNdevice *);
REAL(nvnSyncWait, int, const NVNsync *, uint64_t);
REAL(nvnQueueFenceSync, void, NVNqueue *, NVNsync *, int, int);
REAL(nvnQueueWaitSync, NVNboolean, NVNqueue *, const NVNsync *);
REAL(nvnQueueSubmitCommands, void, NVNqueue *, int, const NVNcommandHandle *);
REAL(nvnQueuePresentTexture, void, NVNqueue *, NVNwindow *, int);
REAL(nvnCommandBufferBeginRecording, void, NVNcommandBuffer *);
REAL(nvnCommandBufferEndRecording, NVNcommandHandle, NVNcommandBuffer *);
REAL(nvnCommandBufferReportCounter, void, NVNcommandBuffer *, int, NVNbufferAddress);
REAL(nvnCommandBufferSetRenderTargets, void, NVNcommandBuffer *, int, const NVNtexture *const *, const NVNtextureView *const *, const NVNtexture *, const NVNtextureView *);
REAL(nvnCommandBufferBindProgram, void, NVNcommandBuffer *, const NVNprogram *, int);
REAL(nvnCommandBufferBarrier, void, NVNcommandBuffer *, int);
REAL(nvnCommandBufferDispatchCompute, void, NVNcommandBuffer *, int, int, int);
REAL(nvnCommandBufferDispatchComputeIndirect, void, NVNcommandBuffer *, NVNbufferAddress);
REAL(nvnCommandBufferDrawMeshTasks, void, NVNcommandBuffer *, int, int);
REAL(nvnCommandBufferDrawMeshTasksIndirect, void, NVNcommandBuffer *, NVNbufferAddress);
REAL(nvnCommandBufferDrawArrays, void, NVNcommandBuffer *, int, int, int);
REAL(nvnCommandBufferDrawElements, void, NVNcommandBuffer *, int, int, int, NVNbufferAddress);
REAL(nvnCommandBufferDrawElementsBaseVertex, void, NVNcommandBuffer *, int, int, int, NVNbufferAddress, int);
REAL(nvnCommandBufferDrawArraysInstanced, void, NVNcommandBuffer *, int, int, int, int, int);
REAL(nvnCommandBufferDrawElementsInstanced, void, NVNcommandBuffer *, int, int, int, NVNbufferAddress, int, int, int);
REAL(nvnCommandBufferDrawArraysIndirect, void, NVNcommandBuffer *, int, NVNbufferAddress);
REAL(nvnCommandBufferDrawElementsIndirect, void, NVNcommandBuffer *, int, int, NVNbufferAddress, NVNbufferAddress);
REAL(nvnCommandBufferMultiDrawArraysIndirectCount, void, NVNcommandBuffer *, int, NVNbufferAddress, NVNbufferAddress, int, ptrdiff_t);
REAL(nvnCommandBufferMultiDrawElementsIndirectCount, void, NVNcommandBuffer *, int, int, NVNbufferAddress, NVNbufferAddress, NVNbufferAddress, int, ptrdiff_t);
REAL(nvnCommandBufferWaitSync, void, NVNcommandBuffer *, const NVNsync *);
REAL(nvnCommandBufferFenceSync, void, NVNcommandBuffer *, NVNsync *, int, int);
REAL(nvnCommandBufferWaitEvent, void, NVNcommandBuffer *, const NVNevent *, int, uint32_t);
REAL(nvnCommandBufferSignalEvent, void, NVNcommandBuffer *, const NVNevent *, int, int, int, uint32_t);
REAL(nvnCommandBufferSetRenderEnable, void, NVNcommandBuffer *, uint32_t);
REAL(nvnCommandBufferSetRenderEnableConditional, void, NVNcommandBuffer *, int, NVNbufferAddress);
REAL(nvnCommandBufferClearColor, void, NVNcommandBuffer *, int, const float *, int);
REAL(nvnCommandBufferClearDepthStencil, void, NVNcommandBuffer *, float, uint32_t, int, int);
REAL(nvnCommandBufferCopyBufferToBuffer, void, NVNcommandBuffer *, NVNbufferAddress, NVNbufferAddress, size_t, int);
REAL(nvnCommandBufferCopyBufferToTexture, void, NVNcommandBuffer *, NVNbufferAddress, const NVNtexture *, const NVNtextureView *, const NVNcopyRegion *, int);
REAL(nvnCommandBufferCopyTextureToBuffer, void, NVNcommandBuffer *, const NVNtexture *, const NVNtextureView *, const NVNcopyRegion *, NVNbufferAddress, int);
REAL(nvnCommandBufferCopyTextureToTexture, void, NVNcommandBuffer *, const NVNtexture *, const NVNtextureView *, const NVNcopyRegion *, const NVNtexture *, const NVNtextureView *, const NVNcopyRegion *, int);
REAL(nvnCommandBufferSetCopyRowStride, void, NVNcommandBuffer *, ptrdiff_t);
REAL(nvnCommandBufferSetCopyImageStride, void, NVNcommandBuffer *, ptrdiff_t);

// ---------------------------------------------------------------- a small lock (never on the per-draw path) and pointer hashing
struct Spin { std::atomic_flag f = ATOMIC_FLAG_INIT; void lock() { while (f.test_and_set(std::memory_order_acquire)) {} } void unlock() { f.clear(std::memory_order_release); } };
static Spin g_mu;
struct Guard { Guard() { g_mu.lock(); } ~Guard() { g_mu.unlock(); } };
static inline uint32_t hptr(const void *p) { uint64_t v = (uint64_t)(uintptr_t)p; v ^= v >> 33; v *= 0xff51afd7ed558ccdull; v ^= v >> 33; return (uint32_t)v; }
static const uint32_t NONE = 0xffffffffu;

// ---------------------------------------------------------------- memory pools: GPU range -> CPU mapping (shader code for the keys, game timestamps)
struct PoolRange { const NVNmemoryPool *pool; NVNbufferAddress gpu; size_t size; const uint8_t *cpu; };
enum { MAXPOOL = 8192 };
static PoolRange g_pools[MAXPOOL]; static int g_npool;
static const uint8_t *gpu_to_cpu(NVNbufferAddress a, size_t n) {   // under g_mu
    for (int i = 0; i < g_npool; i++) { const PoolRange &r = g_pools[i]; if (r.cpu && a >= r.gpu && a + n <= r.gpu + r.size) return r.cpu + (a - r.gpu); }
    return nullptr;
}

// ---------------------------------------------------------------- programs and their keys
enum { NVN2_CTL = 0x48 };
struct Stage { NVNbufferAddress code; uint64_t key; uint8_t state; uint8_t ctl[NVN2_CTL]; };
enum { S_EMPTY, S_DEFER, S_KEYED, S_NOKEY };
struct Prog { std::atomic<const NVNprogram *> p; uint32_t stage0; uint8_t n; };
enum { MAXPROG = 1 << 15, MAXSTAGE = 1 << 15 };
static Prog g_prog[MAXPROG];
static Stage *g_stage; static uint32_t g_nstage, g_stage_overflow;

static int ctl_is_nvn2(const uint8_t *c) { uint32_t magic, ver; memcpy(&magic, c, 4); memcpy(&ver, c + 4, 4); return magic == 0x98761234u && ver == 0xc9; }
static int hash_key(const uint8_t *ctl, uint32_t nctl, NVNbufferAddress data, uint32_t ncode, uint64_t *out) {   // under g_mu
    if (!ncode || ncode >= (1u << 24)) return 0;
    const uint8_t *code = gpu_to_cpu(data, ncode); if (!code) return 0;   // code in a pool the CPU cannot read (or not registered): no key
    st_sha256 h; st_sha256_init(&h); st_sha256_update(&h, ctl, nctl); st_sha256_update(&h, code, ncode);
    uint8_t md[32]; st_sha256_final(&h, md);
    uint64_t k = 0; for (int i = 0; i < 8; i++) k = k << 8 | md[i]; *out = k; return 1;
}
static void stage_set(Stage &s, NVNbufferAddress data, const void *control) {   // under g_mu
    s.code = data; s.key = 0; s.state = S_NOKEY;
    const uint8_t *c = (const uint8_t *)control; if (!c) return;
    if (ctl_is_nvn2(c)) { memcpy(s.ctl, c, NVN2_CTL); memset(s.ctl + 0x30, 0, 4); s.state = S_DEFER; return; }
    uint32_t ncode, own; memcpy(&ncode, c + 0x704, 4); memcpy(&own, c + 0x20, 4);
    uint32_t nctl = own >= 0x760 && own < 0x880 ? own : 0x880;
    static uint8_t ctl[0x880]; memcpy(ctl, c, nctl);
    if (g_cfg.key_skip_len && g_cfg.key_skip_off + g_cfg.key_skip_len <= nctl) memset(ctl + g_cfg.key_skip_off, 0, g_cfg.key_skip_len);
    s.state = hash_key(ctl, nctl, data, ncode, &s.key) ? S_KEYED : S_NOKEY;
}
static uint64_t stage_key(Stage &s) {   // under g_mu; 0 = none
    if (s.state == S_DEFER) { uint32_t ncode; memcpy(&ncode, s.ctl + 0x44, 4); s.state = hash_key(s.ctl, NVN2_CTL, s.code, ncode, &s.key) ? S_KEYED : S_NOKEY; }
    return s.state == S_KEYED ? s.key : 0;
}
static uint32_t prog_find(const NVNprogram *p) {   // 1-based index, 0 = unknown; lock-free (entries are only added, p published last)
    uint32_t h = hptr(p) & (MAXPROG - 1);
    for (int i = 0; i < MAXPROG; i++, h = (h + 1) & (MAXPROG - 1)) {
        const NVNprogram *q = g_prog[h].p.load(std::memory_order_acquire);
        if (q == p) return h + 1;
        if (!q) return 0;
    }
    return 0;
}
static void prog_set(NVNprogram *p, int n, const NVNbufferAddress *data, const void *const *control) {
    if (!g_stage || n <= 0 || n > 6) return;
    Guard g;
    uint32_t h = hptr(p) & (MAXPROG - 1), k = 0;
    for (; k < MAXPROG; k++, h = (h + 1) & (MAXPROG - 1)) { const NVNprogram *q = g_prog[h].p.load(std::memory_order_relaxed); if (q == p || !q) break; }
    if (k == MAXPROG) return;
    Prog &e = g_prog[h];
    uint32_t s0 = NONE;   // a program set again gets new stage records (a capture may still name the old ones); past the table its keys are "?"
    if (g_nstage + (uint32_t)n <= MAXSTAGE) { s0 = g_nstage; g_nstage += (uint32_t)n; for (int i = 0; i < n; i++) stage_set(g_stage[s0 + i], data[i], control[i]); }
    else g_stage_overflow++;
    e.stage0 = s0; e.n = (uint8_t)n;
    if (!e.p.load(std::memory_order_relaxed)) e.p.store(p, std::memory_order_release);
}
static int prog_has_key(uint32_t prog, const uint64_t *keys, int nkeys) {   // dump mode: hashes the program's stages now if needed
    if (!prog || !nkeys) return 0;
    Guard g; const Prog &p = g_prog[prog - 1]; if (p.stage0 == NONE) return 0;
    for (int i = 0; i < p.n; i++) { uint64_t k = stage_key(g_stage[p.stage0 + i]); for (int j = 0; j < nkeys; j++) if (k && k == keys[j]) return 1; }
    return 0;
}
static void prog_keys(uint32_t prog, char *out, size_t cap, uint64_t *seen, int *ns, int *unknown) {   // under g_mu: appends the program's keys not yet in `seen`
    size_t k = strlen(out);
    if (!prog) { *unknown = 1; return; }
    const Prog &p = g_prog[prog - 1];
    if (p.stage0 == NONE) { *unknown = 1; return; }
    for (int i = 0; i < p.n; i++) {
        uint64_t key = stage_key(g_stage[p.stage0 + i]);
        if (!key) { *unknown = 1; continue; }
        bool dup = false; for (int j = 0; j < *ns; j++) dup |= seen[j] == key;
        if (dup || *ns == 64) continue;
        seen[(*ns)++] = key;
        if (k + 18 < cap) k += (size_t)snprintf(out + k, cap - k, "%s%016llx", k ? "," : "", (unsigned long long)key);
    }
}

// ---------------------------------------------------------------- per command buffer
// The table is read without the lock (entries only added; cb published last). NVN records a command buffer on one thread at a time, so its state needs no lock
struct CmdState {
    std::atomic<const NVNcommandBuffer *> cb;
    uint8_t rec;                                  // recording began inside the capture
    uint8_t open_kind;                            // the open interval's kind (also without timing: dumps and the lazy render boundary use it)
    uint32_t open_draws;                          // draws and mesh draws in the open interval (dump_all skips intervals that drew nothing)
    uint32_t first, last, open;                   // timing: first and last stamps of this recording, the open interval's stamp (NONE: none)
    uint32_t ev_first, ev_last;                   // seq: first and last events of this recording
    uint32_t gprog[8], cprog;                     // bound programs (1-based) per graphics stage bit, and compute
    uint32_t ggen, cgen, open_ggen, open_cgen;    // bind generations, and those already noted in the open interval
    const NVNtexture *rt[5];                      // the render targets (colour 0-3, depth) last set
    uint8_t colors; uint16_t w, h; int16_t cfmt[4], dfmt;
    uint8_t dump_pending;                         // dump: the open interval used a dump key
    uint8_t ndumps; uint16_t dumps[16];           // dump: the dumps recorded in this recording (their handle is set at EndRecording)
    ptrdiff_t row_stride, image_stride;           // the game's copy strides (restored after a dump's copy)
};
enum { MAXCMD = 2048 };
static CmdState g_cmd[MAXCMD];
static CmdState *cmd_state(const NVNcommandBuffer *cb) {
    uint32_t h0 = hptr(cb) & (MAXCMD - 1), h = h0;
    for (int i = 0; i < MAXCMD; i++, h = (h + 1) & (MAXCMD - 1)) {
        const NVNcommandBuffer *q = g_cmd[h].cb.load(std::memory_order_acquire);
        if (q == cb) return &g_cmd[h];
        if (!q) break;
    }
    Guard g; h = h0;
    for (int i = 0; i < MAXCMD; i++, h = (h + 1) & (MAXCMD - 1)) {
        const NVNcommandBuffer *q = g_cmd[h].cb.load(std::memory_order_relaxed);
        if (q == cb) return &g_cmd[h];
        if (!q) { CmdState &s = g_cmd[h]; s.rec = 0; s.open = s.last = s.first = s.ev_first = s.ev_last = NONE; s.cb.store(cb, std::memory_order_release); return &s; }
    }
    return nullptr;
}
static std::atomic<bool> g_armed{false};
static inline CmdState *armed_state(NVNcommandBuffer *cb) {   // the state of a command buffer recording inside the capture, or null (the fast path when idle)
    if (!g_armed.load(std::memory_order_relaxed)) return nullptr;
    CmdState *s = cmd_state(cb);
    return s && s->rec ? s : nullptr;
}
// handle -> its first stamp and event, and how often the capture submitted it
struct HandleRange { NVNcommandHandle h; uint32_t first, ev_first, submits; };
enum { MAXHANDLE = 8192 };
static HandleRange g_hr[MAXHANDLE];
static void handle_put(NVNcommandHandle h, uint32_t first, uint32_t ev_first) {   // under g_mu
    uint32_t i = hptr((void *)(uintptr_t)h) & (MAXHANDLE - 1);
    for (int k = 0; k < MAXHANDLE; k++, i = (i + 1) & (MAXHANDLE - 1)) if (!g_hr[i].h || g_hr[i].h == h) { g_hr[i] = {h, first, ev_first, 0}; return; }
}
static HandleRange *handle_get(NVNcommandHandle h) {   // under g_mu
    uint32_t i = hptr((void *)(uintptr_t)h) & (MAXHANDLE - 1);
    for (int k = 0; k < MAXHANDLE; k++, i = (i + 1) & (MAXHANDLE - 1)) { if (g_hr[i].h == h) return &g_hr[i]; if (!g_hr[i].h) return nullptr; }
    return nullptr;
}

// ---------------------------------------------------------------- timing: stamps and the intervals they open
enum { K_BEGIN = 'B', K_R = 'R', K_C = 'C', K_M = 'M', K_END = 'E' };
enum { STAMPS = 1 << 17 };
struct Mark {
    uint8_t kind, colors, indirect, pad;
    uint16_t w, h; int16_t cfmt[4], dfmt;
    uint16_t draws, dispatches, barriers, waits, signals, pad2;
    uint32_t gx, gy, gz, groups;                  // the first dispatch's workgroups; the run's total (dispatches) or task count (mesh draws)
    uint32_t next;                                // the same command buffer's next stamp
    uint32_t binds;                               // head of the interval's program list in g_bind
};
struct Bind { uint32_t prog, next; };
enum { MAXBIND = 1 << 18 };
static NVNmemoryPool g_pool; static uint8_t *g_pool_mem; static NVNbufferAddress g_pool_gpu; static bool g_pool_ok;
static Mark *g_mark; static Bind *g_bind;
static std::atomic<uint32_t> g_next{0}, g_nbind{0}, g_full{0}, g_bind_full{0};

static void note_prog(Mark &m, uint32_t prog) {
    if (!prog) return;
    uint32_t b = g_nbind.fetch_add(1, std::memory_order_relaxed);
    if (b >= MAXBIND) { g_bind_full.store(1, std::memory_order_relaxed); return; }
    g_bind[b] = {prog, m.binds}; m.binds = b;
}
// The stages a use reads: an ordinary draw vertex .. fragment (bits 0-4), a mesh draw fragment, task and mesh (bits 1, 6, 7), a dispatch compute
enum { U_COMPUTE = 0, U_DRAW = 0x1f, U_MESH = 0xc2 };
static void dump_check(CmdState *s, int use);
static inline Mark *open_mark(CmdState *s) { return (g_cfg.modes & M_TIMING) && s->open != NONE ? &g_mark[s->open] : nullptr; }
static inline void note_use(CmdState *s, int use) {
    if (g_cfg.modes & M_DUMP) dump_check(s, use);
    Mark *mp = open_mark(s); if (!mp) return;
    Mark &m = *mp;
    if (use == U_COMPUTE) { if (s->open_cgen != s->cgen) { s->open_cgen = s->cgen; note_prog(m, s->cprog); } return; }
    if (s->open_ggen == s->ggen) return;
    s->open_ggen = s->ggen;
    uint32_t seen[8]; int n = 0;
    for (int i = 0; i < 8; i++) { uint32_t p = use >> i & 1 ? s->gprog[i] : 0; if (!p) continue; bool dup = false; for (int j = 0; j < n; j++) dup |= seen[j] == p; if (!dup) { seen[n++] = p; note_prog(m, p); } }
}
static void dump_at_boundary(NVNcommandBuffer *cb, CmdState *s);
// a boundary in cb: the open interval ends and one of `kind` begins (K_END: none). With timing, a stamp; dumps of the interval that ends
static void stamp(NVNcommandBuffer *cb, CmdState *s, uint8_t kind) {
    if (g_cfg.modes & M_DUMP) dump_at_boundary(cb, s);
    s->open_kind = kind == K_END ? 0 : kind; s->open_ggen = s->open_cgen = 0; s->open_draws = 0;
    if (!(g_cfg.modes & M_TIMING)) { s->open = kind == K_END ? NONE : 0; return; }   // without timing, open only says whether an interval is open
    uint32_t i = g_next.fetch_add(1, std::memory_order_relaxed);
    if (i >= STAMPS) { g_full.store(1, std::memory_order_relaxed); return; }   // the open interval goes on
    Mark &m = g_mark[i]; memset(&m, 0, sizeof m); m.kind = kind; m.next = NONE; m.binds = NONE;
    m.colors = s->colors; m.w = s->w; m.h = s->h; memcpy(m.cfmt, s->cfmt, sizeof m.cfmt); m.dfmt = s->dfmt;
    real_nvnCommandBufferReportCounter(cb, NVN_COUNTER_TYPE_TIMESTAMP, g_pool_gpu + 16ull * i);
    if (s->last != NONE) g_mark[s->last].next = i; else s->first = i;
    s->last = i;
    s->open = kind == K_END ? NONE : i;
}

// ---------------------------------------------------------------- seq: the ordered command stream
enum { E_BEGIN = 1, E_END, E_TARGETS, E_BIND, E_DRAW, E_MESH, E_DISPATCH, E_BARRIER, E_WAIT_EVENT, E_SIGNAL_EVENT, E_WAIT_SYNC, E_FENCE_SYNC, E_REPORT,
       E_RENDER_ENABLE, E_RENDER_COND, E_CLEAR_COLOR, E_CLEAR_DS, E_COPY_B2B, E_COPY_B2T, E_COPY_T2B, E_COPY_T2T, E_DUMP, E_NOPS };
static const char *const EV_NAMES[E_NOPS] = {"", "begin", "end", "targets", "bind", "draw", "mesh", "dispatch", "barrier", "wait_event", "signal_event", "wait_sync",
                                             "fence_sync", "report", "render_enable", "render_cond", "clear_color", "clear_ds", "copy_b2b", "copy_b2t", "copy_t2b", "copy_t2t", "dump"};
struct Ev { uint32_t next; uint8_t op, sub; uint16_t n; uint32_t a, b; uint64_t c; };   // sub: indirect, mode; n: count of a collapsed run of draws
enum { MAXEV = 1 << 18 };
static Ev *g_ev; static std::atomic<uint32_t> g_nev{0}, g_ev_full{0};
static void ev(CmdState *s, uint8_t op, uint8_t sub = 0, uint32_t a = 0, uint32_t b = 0, uint64_t c = 0) {
    if (!(g_cfg.modes & M_SEQ)) return;
    if ((op == E_DRAW || op == E_MESH) && s->ev_last != NONE) {   // a run of draws of one kind is one event
        Ev &l = g_ev[s->ev_last]; if (l.op == op && l.sub == sub && l.n < 0xffff) { l.n++; return; }
    }
    uint32_t i = g_nev.fetch_add(1, std::memory_order_relaxed);
    if (i >= MAXEV) { g_ev_full.store(1, std::memory_order_relaxed); return; }
    g_ev[i] = {NONE, op, sub, 1, a, b, c};
    if (s->ev_last != NONE) g_ev[s->ev_last].next = i; else s->ev_first = i;
    s->ev_last = i;
}

// ---------------------------------------------------------------- dump: copies of render targets and buffers
struct Dump { uint16_t w, h; int16_t fmt; uint8_t kind; uint32_t bytes; uint64_t off; NVNbufferAddress buf; NVNcommandHandle handle; long frame; };   // kind 0 colour, 1 depth, 2 buffer
// A dump belongs to the frame whose submission runs its command buffer: dumps recorded in the 2 frames before the first logged one, or never submitted, are not written
enum { MAXDUMP = 256 };
static Dump g_dump[MAXDUMP]; static std::atomic<uint32_t> g_ndump{0}, g_dump_skipped{0};
static std::atomic<uint64_t> g_dump_used{0};
static NVNmemoryPool g_dpool; static uint8_t *g_dpool_mem; static NVNbufferAddress g_dpool_gpu; static size_t g_dpool_size; static bool g_dpool_ok;
static void dump_check(CmdState *s, int use) {
    if (s->dump_pending || !g_cfg.ndump_keys) return;
    if (use == U_COMPUTE) { if (prog_has_key(s->cprog, g_cfg.dump_keys, g_cfg.ndump_keys)) s->dump_pending = 1; return; }
    for (int i = 0; i < 8; i++) if (use >> i & 1 && prog_has_key(s->gprog[i], g_cfg.dump_keys, g_cfg.ndump_keys)) { s->dump_pending = 1; return; }
}
static uint32_t dump_alloc(CmdState *s, uint32_t bytes) {
    if (s->ndumps >= 16) { g_dump_skipped.fetch_add(1); return NONE; }
    uint32_t i = g_ndump.fetch_add(1);
    if (i >= MAXDUMP || i >= (uint32_t)g_cfg.dump_max) { g_ndump.fetch_sub(1); g_dump_skipped.fetch_add(1); return NONE; }
    uint64_t off = g_dump_used.fetch_add((bytes + 255) & ~255u);
    if (off + bytes > g_dpool_size) { g_dump[i] = {0, 0, 0, 3, 0, 0, 0, 0, -1}; g_dump_skipped.fetch_add(1); return NONE; }   // kind 3: an empty slot (dump_mb ran out)
    g_dump[i].off = off; g_dump[i].bytes = bytes; g_dump[i].handle = 0; g_dump[i].frame = -1;
    s->dumps[s->ndumps++] = (uint16_t)i;
    return i;
}
static void dump_texture(NVNcommandBuffer *cb, CmdState *s, const NVNtexture *t, int fmt, uint8_t kind) {
    int bpp = format_bpp(fmt); if (!t || !bpp || !s->w || !s->h) { g_dump_skipped.fetch_add(1); return; }
    uint32_t i = dump_alloc(s, (uint32_t)s->w * s->h * bpp); if (i == NONE) return;
    Dump &d = g_dump[i]; d.w = s->w; d.h = s->h; d.fmt = (int16_t)fmt; d.kind = kind; d.buf = 0;
    NVNcopyRegion r = {0, 0, 0, s->w, s->h, 1};
    real_nvnCommandBufferSetCopyRowStride(cb, 0); real_nvnCommandBufferSetCopyImageStride(cb, 0);   // tightly packed rows
    real_nvnCommandBufferCopyTextureToBuffer(cb, t, nullptr, &r, g_dpool_gpu + d.off, 0);
    real_nvnCommandBufferSetCopyRowStride(cb, s->row_stride); real_nvnCommandBufferSetCopyImageStride(cb, s->image_stride);
    ev(s, E_DUMP, kind, i);
}
// at the end of an interval: copy its colour target 0 (and depth) when it used a dump key, or every render interval with dump_all
static void dump_at_boundary(NVNcommandBuffer *cb, CmdState *s) {
    bool render = s->open_kind == K_R || s->open_kind == K_M || s->open_kind == K_BEGIN;
    bool want = s->dump_pending || (g_cfg.dump_all && render && s->open != NONE && s->open_draws && s->colors);
    s->dump_pending = 0;
    if (!want || !g_dpool_ok) return;
    if (g_cfg.dump_w && (s->w != g_cfg.dump_w || s->h != g_cfg.dump_h)) return;
    real_nvnCommandBufferBarrier(cb, NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_INVALIDATE_TEXTURE_BIT);   // the pass's writes done before the copy reads them
    if (s->rt[0]) dump_texture(cb, s, s->rt[0], s->cfmt[0], 0);
    if (g_cfg.dump_depth && s->rt[4]) dump_texture(cb, s, s->rt[4], s->dfmt, 1);
    for (int b = 0; b < g_cfg.ndump_buf; b++) {
        uint32_t i = dump_alloc(s, g_cfg.dump_buf[b].size); if (i == NONE) continue;
        Dump &d = g_dump[i]; d.w = d.h = 0; d.fmt = 0; d.kind = 2; d.buf = g_cfg.dump_buf[b].addr;
        real_nvnCommandBufferCopyBufferToBuffer(cb, g_cfg.dump_buf[b].addr, g_dpool_gpu + d.off, g_cfg.dump_buf[b].size, 0);
        ev(s, E_DUMP, 2, i);
    }
}

// ---------------------------------------------------------------- captures
enum { ST_IDLE, ST_PENDING, ST_ARMED, ST_DRAIN };   // pending: triggered, recording not yet on (it starts 2 frames before the first logged one)
static int g_state = ST_IDLE;
struct SubRange { long frame; uint16_t queue; uint8_t dup, unstamped; uint32_t first, ev_first; NVNcommandHandle h; };
enum { MAXSUB = 1 << 14, MAXQEV = 1 << 12, MAXQ = 8 };
static SubRange g_sub[MAXSUB]; static int g_nsub;
struct QueueEv { long frame; uint32_t at_sub; uint16_t queue; char what; const void *obj; };   // queue-level waits ('W'), fences ('F') and presents ('P') before submission at_sub
static QueueEv g_qev[MAXQEV]; static int g_nqev;
static NVNqueue *g_queues[MAXQ]; static int g_nqueue;
static NVNsync g_sync[MAXQ]; static bool g_sync_ok[MAXQ], g_fenced[MAXQ];
static NVNdevice *g_dev;
static long g_present, g_log_from = -1, g_log_to = -1, g_drain_from;
static int g_capture_no;
static uint32_t g_combo_was;
// the game's own counter reports while frames are logged: counts per type, and where its timestamps go
enum { NCTYPE = 18, MAXGTS = 32 };
static std::atomic<uint32_t> g_gctr[NCTYPE];
struct GameTs { NVNbufferAddress addr; int type; uint32_t n; };
static GameTs g_gts[MAXGTS]; static int g_ngts;

static int queue_id(NVNqueue *q) {   // under g_mu
    for (int i = 0; i < g_nqueue; i++) if (g_queues[i] == q) return i;
    if (g_nqueue < MAXQ) { g_queues[g_nqueue] = q; return g_nqueue++; }
    return MAXQ - 1;
}
static bool pool_make(NVNmemoryPool *pool, uint8_t **mem, NVNbufferAddress *gpu, size_t n, const char *what) {   // CPU uncached (no invalidate before reading), GPU cached
    *mem = (uint8_t *)aligned_alloc(4096, n);
    if (!*mem) { st_log("switch_trace: no memory for the %s pool (%zu bytes)", what, n); return false; }
    memset(*mem, 0, n);
    NVNmemoryPoolBuilder b; memset(&b, 0, sizeof b); real_nvnMemoryPoolBuilderSetDefaults(&b); real_nvnMemoryPoolBuilderSetDevice(&b, g_dev);
    real_nvnMemoryPoolBuilderSetFlags(&b, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    real_nvnMemoryPoolBuilderSetStorage(&b, *mem, n);
    if (!real_nvnMemoryPoolInitialize(pool, &b)) { st_log("switch_trace: %s pool initialize failed", what); free(*mem); *mem = nullptr; return false; }
    *gpu = real_nvnMemoryPoolGetBufferAddress(pool);
    return true;
}
static bool capture_init(void) {   // at the first capture: the pools and tables its modes need
    if (!g_dev) { st_log("switch_trace: no NVN device yet"); return false; }
    static bool syncs; if (!syncs) { for (int i = 0; i < MAXQ; i++) g_sync_ok[i] = real_nvnSyncInitialize(&g_sync[i], g_dev); syncs = true; }
    if ((g_cfg.modes & M_TIMING) && !g_pool_ok) {
        if (!g_mark) g_mark = (Mark *)calloc(STAMPS, sizeof(Mark));
        if (!g_bind) g_bind = (Bind *)calloc(MAXBIND, sizeof(Bind));
        if (!g_mark || !g_bind) { st_log("switch_trace: no memory for the timing tables"); return false; }
        if (!(g_pool_ok = pool_make(&g_pool, &g_pool_mem, &g_pool_gpu, 16ull * STAMPS, "counter"))) return false;
    }
    if ((g_cfg.modes & M_SEQ) && !g_ev) { g_ev = (Ev *)calloc(MAXEV, sizeof(Ev)); if (!g_ev) { st_log("switch_trace: no memory for the sequence log"); return false; } }
    if ((g_cfg.modes & M_DUMP) && !g_dpool_ok) {
        g_dpool_size = (size_t)g_cfg.dump_mb << 20;
        if (!(g_dpool_ok = pool_make(&g_dpool, &g_dpool_mem, &g_dpool_gpu, g_dpool_size, "dump"))) return false;
    }
    return true;
}

static uint32_t parse_combo(const char *v) {   // "LS+RS", "ZL+ZR+Minus", "off"
    if (!strcmp(v, "0") || !strcasecmp(v, "off")) return 0;
    uint32_t m = 0; char buf[96]; snprintf(buf, sizeof buf, "%s", v); char *save = nullptr;
    for (char *t = strtok_r(buf, "+ \t", &save); t; t = strtok_r(nullptr, "+ \t", &save)) {
        int found = 0;
        for (int i = 0; i < 16; i++) if (!strcasecmp(t, BUTTON_NAMES[i])) { m |= 1u << i; found = 1; }
        if (!found) st_log("switch_trace: unknown button '%s' in combo (names: A B X Y LS RS L R ZL ZR Plus Minus Left Up Right Down)", t);
    }
    return m;
}
static void load_config(void) {
    char b[4096]; char path[160]; snprintf(path, sizeof path, "%s/config.txt", DIR);
    if (st_fs_read_small(path, b, sizeof b) < 0) return;
    char *save = nullptr;
    for (char *l = strtok_r(b, "\r\n", &save); l; l = strtok_r(nullptr, "\r\n", &save)) {
        char *hash = strchr(l, '#'); if (hash) *hash = 0;
        char *e = l + strlen(l); while (e > l && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        while (*l == ' ' || *l == '\t') l++;
        if (!*l) continue;
        char *v = strchr(l, '='); if (!v) { st_log("switch_trace: config line without '=': '%s'", l); continue; }
        *v++ = 0;
        if (!strcmp(l, "mode")) {
            g_cfg.modes = 0; char *s2 = nullptr;
            for (char *t = strtok_r(v, ", ", &s2); t; t = strtok_r(nullptr, ", ", &s2)) {
                if (!strcmp(t, "timing")) g_cfg.modes |= M_TIMING; else if (!strcmp(t, "seq")) g_cfg.modes |= M_SEQ; else if (!strcmp(t, "dump")) g_cfg.modes |= M_DUMP;
                else st_log("switch_trace: unknown mode '%s' (timing, seq, dump)", t);
            }
        }
        else if (!strcmp(l, "start_frame")) g_cfg.start_frame = atol(v);
        else if (!strcmp(l, "frames")) g_cfg.frames = atol(v);
        else if (!strcmp(l, "delay")) g_cfg.delay = atol(v);
        else if (!strcmp(l, "gpu_ts_hz")) g_cfg.gpu_ts_hz = atof(v);
        else if (!strcmp(l, "combo")) g_cfg.combo = parse_combo(v);
        else if (!strcmp(l, "split")) g_cfg.each = !strcmp(v, "each");
        else if (!strcmp(l, "key_skip")) { char *c = strchr(v, ':'); g_cfg.key_skip_off = (uint32_t)strtoul(v, nullptr, 0); g_cfg.key_skip_len = c ? (uint32_t)strtoul(c + 1, nullptr, 0) : 0; }
        else if (!strcmp(l, "ext_stride")) g_cfg.ext_stride = (uint32_t)strtoul(v, nullptr, 0);
        else if (!strcmp(l, "dump_keys")) {
            char *s2 = nullptr;
            for (char *t = strtok_r(v, ", ", &s2); t && g_cfg.ndump_keys < MAXDUMPKEY; t = strtok_r(nullptr, ", ", &s2)) g_cfg.dump_keys[g_cfg.ndump_keys++] = strtoull(t, nullptr, 16);
        }
        else if (!strcmp(l, "dump_all")) g_cfg.dump_all = atoi(v);
        else if (!strcmp(l, "dump_depth")) g_cfg.dump_depth = atoi(v);
        else if (!strcmp(l, "dump_max")) g_cfg.dump_max = atoi(v);
        else if (!strcmp(l, "dump_mb")) g_cfg.dump_mb = atol(v);
        else if (!strcmp(l, "dump_size")) { char *x = strchr(v, 'x'); g_cfg.dump_w = (uint16_t)atoi(v); g_cfg.dump_h = x ? (uint16_t)atoi(x + 1) : 0; }
        else if (!strcmp(l, "dump_buffer")) {
            char *c = strchr(v, ':');
            if (c && g_cfg.ndump_buf < MAXDUMPBUF) { g_cfg.dump_buf[g_cfg.ndump_buf].addr = strtoull(v, nullptr, 16); g_cfg.dump_buf[g_cfg.ndump_buf++].size = (uint32_t)strtoul(c + 1, nullptr, 0); }
        }
        else st_log("switch_trace: unknown config key '%s'", l);
    }
    if (g_cfg.frames < 1) g_cfg.frames = 1;
    if (g_cfg.delay < 2) g_cfg.delay = 2;   // command buffers are often recorded a frame ahead
    if (g_cfg.dump_max > MAXDUMP) g_cfg.dump_max = MAXDUMP;
    if (g_cfg.dump_mb < 1) g_cfg.dump_mb = 1;
}
static const char *modes_str(void) {
    static char b[32]; b[0] = 0;
    if (g_cfg.modes & M_TIMING) strcat(b, "timing");
    if (g_cfg.modes & M_SEQ) strcat(b, *b ? ",seq" : "seq");
    if (g_cfg.modes & M_DUMP) strcat(b, *b ? ",dump" : "dump");
    return *b ? b : "none";
}
static void trigger(long first) {   // the logged frames are first .. first + frames - 1; recording starts 2 frames earlier
    if (g_state != ST_IDLE || !g_cfg.modes) return;
    g_log_from = first; g_log_to = first + g_cfg.frames - 1; g_state = ST_PENDING;
}
static void arm(void) {
    if (!capture_init()) { g_state = ST_IDLE; g_log_from = g_log_to = -1; return; }
    g_next.store(0); g_nbind.store(0); g_full.store(0); g_bind_full.store(0); g_nev.store(0); g_ev_full.store(0);
    g_ndump.store(0); g_dump_used.store(0); g_dump_skipped.store(0);
    for (auto &c : g_gctr) c.store(0);
    {
        Guard g; g_nsub = 0; g_nqev = 0; g_ngts = 0;
        for (auto &c : g_cmd) if (c.cb.load(std::memory_order_relaxed)) { c.rec = 0; c.open = c.first = c.last = c.ev_first = c.ev_last = NONE; c.dump_pending = 0; }
        for (auto &h : g_hr) h = {};
    }
    g_capture_no++;
    g_state = ST_ARMED; g_armed.store(true);
    st_log("switch_trace: capture %d armed at present %ld, logging frames %ld-%ld (mode=%s, split=%s)", g_capture_no, g_present, g_log_from, g_log_to, modes_str(), g_cfg.each ? "each" : "run");
}
static bool logging_now(void) { return g_state == ST_ARMED && g_present >= g_log_from && g_present <= g_log_to; }

// ---------------------------------------------------------------- writing a capture (after its fences signalled)
struct Out {   // a file written in pieces of up to 256 KiB
    char buf[1 << 18]; size_t n = 0; char path[192]; size_t total = 0;
    void open(const char *name) { n = total = 0; snprintf(path, sizeof path, "%s/%s", DIR, name); st_fs_delete(path); }
    void flush() { if (n) { st_fs_append(path, buf, n); total += n; n = 0; } }
    __attribute__((format(printf, 2, 3))) void put(const char *fmt, ...) {
        if (n > sizeof buf - 4096) flush();
        va_list v; va_start(v, fmt); int k = vsnprintf(buf + n, sizeof buf - n, fmt, v); va_end(v);
        if (k > 0) n += (size_t)k < sizeof buf - n ? (size_t)k : sizeof buf - n - 1;
    }
};
static Out g_out;
static uint64_t ts_ns(uint32_t slot) {
    NVNcounterData d; memcpy(&d, g_pool_mem + 16ull * slot, sizeof d);
    if (real_nvnDeviceGetTimestampInNanoseconds) return real_nvnDeviceGetTimestampInNanoseconds(g_dev, &d);
    return g_cfg.gpu_ts_hz > 0 ? (uint64_t)((double)d.timestamp * 1e9 / g_cfg.gpu_ts_hz) : d.timestamp;
}
static const char *ts_unit(void) { return real_nvnDeviceGetTimestampInNanoseconds ? "nvnDeviceGetTimestampInNanoseconds" : g_cfg.gpu_ts_hz > 0 ? "gpu_ts_hz" : "raw ticks (the us columns are ticks / 1000)"; }
struct Ids { const void *p[256]; int n = 0; int id(const void *q) { for (int i = 0; i < n; i++) if (p[i] == q) return i; if (n < 256) { p[n] = q; return n++; } return 255; } };   // small numbers for events and syncs
static void footer_common(Out &o, double ms) {
    long nf = g_log_to - g_log_from + 1; char b[512]; size_t k = 0;
    for (int t = 0; t < NCTYPE; t++) { uint32_t n = g_gctr[t].load(); if (n) k += (size_t)snprintf(b + k, sizeof b - k, " %s%d=%.1f", t == NCTYPE - 1 ? "other" : "type", t == NCTYPE - 1 ? 0 : t, n / (double)nf); }
    o.put("# game ReportCounter a frame by counter type (0 TIMESTAMP, 1 SAMPLES_PASSED, 16 TIMESTAMP_TOP):%s\n", k ? b : " none");
    for (int i = 0; i < g_ngts; i++) o.put("# game timestamp %s at %#llx: %.2f a frame; CPU-readable %s\n", g_gts[i].type ? "TIMESTAMP_TOP" : "TIMESTAMP",
                                           (unsigned long long)g_gts[i].addr, g_gts[i].n / (double)nf, gpu_to_cpu(g_gts[i].addr, 16) ? "yes" : "no");
    o.put("# stage records %u of %u (%u programs past the table: keys \"?\"); written in %.1f ms after the capture\n", g_nstage, (unsigned)MAXSTAGE, g_stage_overflow, ms);
}
static void write_qev(Out &o, int &qe, long frame, uint32_t upto_sub, int &idx, Ids &syncs, bool seq) {   // queue events of `frame` before submission upto_sub
    for (; qe < g_nqev && g_qev[qe].frame == frame && g_qev[qe].at_sub <= upto_sub; qe++) {
        const QueueEv &q = g_qev[qe];
        if (seq) {
            if (q.what == 'P') o.put("%ld\t%d\t%u\t\t\tpresent\t\t\n", frame, idx++, q.queue);
            else o.put("%ld\t%d\t%u\t\t\t%s\tsync %d\t\n", frame, idx++, q.queue, q.what == 'W' ? "queue_wait" : "queue_fence", syncs.id(q.obj));
        } else if (q.what != 'P') o.put("%ld\t%d\t\t%c\t%u\t\t\t\t\t\t\t\t\t\t\t\t\t\t\tqueue %s\n", frame, idx++, q.what, q.queue, q.what == 'W' ? "wait" : "fence");
    }
}
static void write_timing(void) {   // under g_mu
    uint64_t t0 = st_cpu_ticks();
    Out &o = g_out; char name[64]; snprintf(name, sizeof name, "capture%d.tsv", g_capture_no); o.open(name);
    o.put("# switch_trace timing, capture %d; frames %ld-%ld; split=%s; timestamps via %s\n", g_capture_no, g_log_from, g_log_to, g_cfg.each ? "each" : "run", ts_unit());
    o.put("frame\tidx\tus\tkind\tqueue\tw\th\tcolors\tcfmt\tdfmt\tdraws\tdispatches\tgroups\tindirect\tkeys\twaits\tsignals\tbarriers\tstart_us\tflags\n");
    static char keys[64 * 17 + 8]; uint64_t seen[64]; Ids syncs;
    long rows = 0;
    for (int s0 = 0, qe = 0; s0 < g_nsub;) {
        long frame = g_sub[s0].frame; int s1 = s0; while (s1 < g_nsub && g_sub[s1].frame == frame) s1++;
        while (qe < g_nqev && g_qev[qe].frame < frame) qe++;
        uint64_t t_first = UINT64_MAX;
        for (int s = s0; s < s1; s++) if (!g_sub[s].unstamped && g_sub[s].first != NONE) { uint64_t t = ts_ns(g_sub[s].first); if (t && t < t_first) t_first = t; }
        int idx = 0;
        for (int s = s0; s < s1; s++) {
            const SubRange &r = g_sub[s];
            write_qev(o, qe, frame, (uint32_t)s, idx, syncs, false);
            if (r.unstamped || r.first == NONE) { o.put("%ld\t%d\t\t?\t%u\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\tunstamped\n", frame, idx++, r.queue); rows++; continue; }
            for (uint32_t i = r.first; i != NONE && i < STAMPS; i = g_mark[i].next) {   // interval i: from stamp i to the command buffer's next stamp
                const Mark &m = g_mark[i];
                if (m.kind == K_END || m.next == NONE || m.next >= STAMPS) continue;
                if (m.kind == K_BEGIN && !m.draws && !m.dispatches) continue;   // the stretch before the first boundary, empty
                uint64_t a = ts_ns(i), b = ts_ns(m.next);
                keys[0] = 0; int ns = 0, unknown = 0;
                for (uint32_t bi = m.binds; bi != NONE && bi < MAXBIND; bi = g_bind[bi].next) prog_keys(g_bind[bi].prog, keys, sizeof keys, seen, &ns, &unknown);
                if (unknown) { size_t k = strlen(keys); snprintf(keys + k, sizeof keys - k, "%s?", k ? "," : ""); }
                char groups[48] = "";
                if (m.kind == K_C && !m.indirect) { if (m.dispatches == 1) snprintf(groups, sizeof groups, "%ux%ux%u", m.gx, m.gy, m.gz); else snprintf(groups, sizeof groups, "%u", m.groups); }
                else if (m.kind == K_M && !m.indirect) snprintf(groups, sizeof groups, "%u", m.groups);
                o.put("%ld\t%d\t%.1f\t%c\t%u\t%u\t%u\t%u\t%d,%d,%d,%d\t%d\t%u\t%u\t%s\t%u\t%s\t%u\t%u\t%u\t%.1f\t%s\n",
                      frame, idx++, b >= a ? (b - a) / 1e3 : -1.0, m.kind == K_BEGIN ? 'R' : m.kind, r.queue, m.w, m.h, m.colors, m.cfmt[0], m.cfmt[1], m.cfmt[2], m.cfmt[3], m.dfmt,
                      m.draws, m.dispatches, groups, m.indirect, keys, m.waits, m.signals, m.barriers, t_first != UINT64_MAX && a >= t_first ? (a - t_first) / 1e3 : -1.0, r.dup ? "dup" : "");
                rows++;
            }
        }
        write_qev(o, qe, frame, NONE, idx, syncs, false);
        s0 = s1;
    }
    uint32_t used = g_next.load(); if (used > STAMPS) used = STAMPS;
    uint32_t nb = g_nbind.load(); if (nb > MAXBIND) nb = MAXBIND;
    o.put("# slots used %u of %u%s; program notes %u of %u%s; %ld rows\n", used, (unsigned)STAMPS, g_full.load() ? " (FULL: later stamps dropped)" : "", nb, (unsigned)MAXBIND,
          g_bind_full.load() ? " (FULL)" : "", rows);
    footer_common(o, (st_cpu_ticks() - t0) * 1e3 / (double)st_cpu_tick_hz());
    o.flush();
    st_log("switch_trace: %s: %ld rows, %zu bytes", o.path, rows, o.total);
}
static void write_seq(void) {   // under g_mu
    uint64_t t0 = st_cpu_ticks();
    Out &o = g_out; char name[64]; snprintf(name, sizeof name, "seq%d.tsv", g_capture_no); o.open(name);
    o.put("# switch_trace sequence log, capture %d; frames %ld-%ld\n", g_capture_no, g_log_from, g_log_to);
    o.put("frame\tidx\tqueue\tcb\tn\top\targs\tkeys\n");
    static char keys[64 * 17 + 8]; uint64_t seen[64]; Ids events, syncs;
    long rows = 0;
    for (int s0 = 0, qe = 0; s0 < g_nsub;) {
        long frame = g_sub[s0].frame; int s1 = s0; while (s1 < g_nsub && g_sub[s1].frame == frame) s1++;
        while (qe < g_nqev && g_qev[qe].frame < frame) qe++;
        int idx = 0;
        for (int s = s0; s < s1; s++) {
            const SubRange &r = g_sub[s];
            write_qev(o, qe, frame, (uint32_t)s, idx, syncs, true);
            o.put("%ld\t%d\t%u\t%d\t\tsubmit\t%s\t\n", frame, idx++, r.queue, s - s0, r.ev_first == NONE ? "unrecorded" : r.dup ? "dup" : "");
            uint32_t gprog[8] = {0}, cprog = 0; int n = 0;   // the bindings as the stream sets them (a recording's first binds carry those it inherited)
            for (uint32_t i = r.ev_first; i != NONE && i < MAXEV; i = g_ev[i].next, n++) {
                const Ev &e = g_ev[i]; char args[160] = ""; keys[0] = 0; int ns = 0, unknown = 0;
                switch (e.op) {
                case E_TARGETS: snprintf(args, sizeof args, "%ux%u colors %u cfmt %d dfmt %d", e.a & 0xffff, e.a >> 16, e.b & 0xff, (int16_t)(e.b >> 8), (int16_t)e.c); break;
                case E_BIND:
                    if (e.a & NVN_SHADER_STAGE_COMPUTE_BIT) cprog = e.b;
                    for (int k = 0; k < 8; k++) if (k != 5 && (e.a >> k & 1)) gprog[k] = e.b;
                    snprintf(args, sizeof args, "stages %#x", e.a); prog_keys(e.b, keys, sizeof keys, seen, &ns, &unknown); break;
                case E_DRAW: case E_MESH: {
                    snprintf(args, sizeof args, "%u%s", e.n, e.sub ? " indirect" : "");
                    uint32_t done[8]; int nd = 0, use = e.op == E_DRAW ? U_DRAW : U_MESH;
                    for (int k = 0; k < 8; k++) {
                        uint32_t p = use >> k & 1 ? gprog[k] : 0; if (!p) continue;
                        bool d = false; for (int j = 0; j < nd; j++) d |= done[j] == p;
                        if (!d) { done[nd++] = p; prog_keys(p, keys, sizeof keys, seen, &ns, &unknown); }
                    }
                    break;
                }
                case E_DISPATCH: if (e.sub) snprintf(args, sizeof args, "indirect"); else snprintf(args, sizeof args, "%ux%ux%u", e.a, e.b, (uint32_t)e.c); prog_keys(cprog, keys, sizeof keys, seen, &ns, &unknown); break;
                case E_BARRIER: snprintf(args, sizeof args, "bits %#x", e.a); break;
                case E_WAIT_EVENT: snprintf(args, sizeof args, "event %d mode %u value %u", events.id((const void *)(uintptr_t)e.c), e.sub, e.b); break;
                case E_SIGNAL_EVENT: snprintf(args, sizeof args, "event %d mode %u location %u flags %u value %u", events.id((const void *)(uintptr_t)e.c), e.sub, e.a & 0xff, e.a >> 8, e.b); break;
                case E_WAIT_SYNC: case E_FENCE_SYNC: snprintf(args, sizeof args, "sync %d", syncs.id((const void *)(uintptr_t)e.c)); break;
                case E_REPORT: snprintf(args, sizeof args, "type %u addr %#llx", e.a, (unsigned long long)e.c); break;
                case E_RENDER_ENABLE: snprintf(args, sizeof args, "%u", e.a); break;
                case E_RENDER_COND: snprintf(args, sizeof args, "mode %u addr %#llx", e.a, (unsigned long long)e.c); break;
                case E_CLEAR_COLOR: snprintf(args, sizeof args, "c%u mask %#x", e.a, e.b); break;
                case E_CLEAR_DS: snprintf(args, sizeof args, "depth_mask %u stencil_mask %#x", e.a, e.b); break;
                case E_COPY_B2B: snprintf(args, sizeof args, "%u bytes", e.a); break;
                case E_COPY_B2T: case E_COPY_T2B: case E_COPY_T2T: snprintf(args, sizeof args, "%ux%u", e.a, e.b); break;
                case E_DUMP: snprintf(args, sizeof args, "%s %u", e.sub == 2 ? "buffer" : e.sub ? "depth" : "color", e.a); break;
                default: break;
                }
                if (unknown) { size_t k = strlen(keys); snprintf(keys + k, sizeof keys - k, "%s?", k ? "," : ""); }
                o.put("%ld\t%d\t%u\t%d\t%d\t%s\t%s\t%s\n", frame, idx++, r.queue, s - s0, n, e.op < E_NOPS ? EV_NAMES[e.op] : "?", args, keys);
                rows++;
            }
        }
        write_qev(o, qe, frame, NONE, idx, syncs, true);
        s0 = s1;
    }
    uint32_t used = g_nev.load(); if (used > MAXEV) used = MAXEV;
    o.put("# events %u of %u%s; %ld rows\n", used, (unsigned)MAXEV, g_ev_full.load() ? " (FULL: later events dropped)" : "", rows);
    footer_common(o, (st_cpu_ticks() - t0) * 1e3 / (double)st_cpu_tick_hz());
    o.flush();
    st_log("switch_trace: %s: %ld rows, %zu bytes", o.path, rows, o.total);
}
static void write_dumps(void) {   // under g_mu: one .bin per dump of a logged frame, and an index
    Out &o = g_out; char name[64]; snprintf(name, sizeof name, "dump%d.tsv", g_capture_no); o.open(name);
    o.put("# switch_trace dumps, capture %d; frames %ld-%ld; files dump%d_<n>.bin (rows tightly packed, top row first)\n", g_capture_no, g_log_from, g_log_to, g_capture_no);
    o.put("n\tframe\tkind\tw\th\tnvn_format\tbpp\tbytes\tbuffer\tfile\n");
    uint32_t nd = g_ndump.load(); if (nd > MAXDUMP) nd = MAXDUMP;
    uint32_t written = 0, outside = 0;
    for (uint32_t i = 0; i < nd; i++) {
        const Dump &d = g_dump[i]; if (d.kind == 3 || !d.bytes) continue;
        if (d.frame < 0) { outside++; continue; }
        char file[64]; snprintf(file, sizeof file, "dump%d_%u.bin", g_capture_no, i);
        char path[192]; snprintf(path, sizeof path, "%s/%s", DIR, file); st_fs_delete(path);
        for (uint32_t off = 0; off < d.bytes; off += 1u << 20) st_fs_append(path, g_dpool_mem + d.off + off, d.bytes - off < (1u << 20) ? d.bytes - off : 1u << 20);
        o.put("%u\t%ld\t%s\t%u\t%u\t%d\t%d\t%u\t%#llx\t%s\n", i, d.frame, d.kind == 2 ? "buffer" : d.kind ? "depth" : "color", d.w, d.h, d.fmt, format_bpp(d.fmt), d.bytes,
              (unsigned long long)d.buf, file);
        written++;
    }
    o.put("# dumps %u; skipped %u (dump_max, dump_mb, or a compressed / unknown format); %u recorded outside the logged frames\n", written, g_dump_skipped.load(), outside);
    o.flush();
    st_log("switch_trace: %s: %u dumps", o.path, written);
}

// ---------------------------------------------------------------- wrappers
static NVNboolean w_nvnDeviceInitialize(NVNdevice *d, const NVNdeviceBuilder *b) { NVNboolean ok = real_nvnDeviceInitialize(d, b); if (ok && !g_dev) g_dev = d; return ok; }
static NVNboolean w_nvnMemoryPoolInitialize(NVNmemoryPool *p, const NVNmemoryPoolBuilder *b) {
    NVNboolean ok = real_nvnMemoryPoolInitialize(p, b);
    if (ok) {
        int f = real_nvnMemoryPoolGetFlags(p); const uint8_t *cpu = (f & (NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_CPU_CACHED)) ? (const uint8_t *)real_nvnMemoryPoolMap(p) : nullptr;
        PoolRange r = {p, real_nvnMemoryPoolGetBufferAddress(p), real_nvnMemoryPoolGetSize(p), cpu};
        Guard g; if (g_npool < MAXPOOL) g_pools[g_npool++] = r;
    }
    return ok;
}
static void w_nvnMemoryPoolFinalize(NVNmemoryPool *p) {
    { Guard g; for (int i = 0; i < g_npool; i++) if (g_pools[i].pool == p) { g_pools[i] = g_pools[--g_npool]; break; } }
    real_nvnMemoryPoolFinalize(p);
}
static NVNboolean w_nvnProgramSetShaders(NVNprogram *p, int n, const NVNshaderData *sd) {
    NVNboolean ok = real_nvnProgramSetShaders(p, n, sd);
    if (ok && n > 0 && n <= 6) { NVNbufferAddress d[6]; const void *c[6]; for (int i = 0; i < n; i++) { d[i] = sd[i].data; c[i] = sd[i].control; } prog_set(p, n, d, c); }
    return ok;
}
static NVNboolean w_nvnProgramSetShadersExt(NVNprogram *p, int n, const void *sd) {   // each stage {data, control, ...} at ext_stride bytes
    NVNboolean ok = real_nvnProgramSetShadersExt(p, n, sd);
    if (ok && n > 0 && n <= 6) {
        NVNbufferAddress d[6]; const void *c[6];
        for (int i = 0; i < n; i++) { const uint8_t *e = (const uint8_t *)sd + (size_t)i * g_cfg.ext_stride; memcpy(&d[i], e, 8); memcpy(&c[i], e + 8, sizeof(void *)); }
        prog_set(p, n, d, c);
    }
    return ok;
}
// NVN2's program builder keeps the caller's array until nvnProgramInitializeFromBuilder (hle_nvn.c): note it per builder, key the program at initialize
struct PendingBuilder { const void *b; int n; const NVNshaderData *sd; };
static PendingBuilder g_pb[64];
static void w_nvnProgramBuilderSetShaders(void *b, int n, const NVNshaderData *sd) { real_nvnProgramBuilderSetShaders(b, n, sd); Guard g; g_pb[hptr(b) & 63] = {b, n, sd}; }
static NVNboolean w_nvnProgramInitializeFromBuilder(NVNprogram *p, const void *b) {
    NVNboolean ok = real_nvnProgramInitializeFromBuilder(p, b);
    PendingBuilder pb; { Guard g; pb = g_pb[hptr(b) & 63]; }
    if (ok && pb.b == b && pb.n > 0 && pb.n <= 6) { NVNbufferAddress d[6]; const void *c[6]; for (int i = 0; i < pb.n; i++) { d[i] = pb.sd[i].data; c[i] = pb.sd[i].control; } prog_set(p, pb.n, d, c); }
    return ok;
}
static void w_nvnCommandBufferBeginRecording(NVNcommandBuffer *cb) {
    real_nvnCommandBufferBeginRecording(cb);
    if (!g_armed.load(std::memory_order_relaxed)) return;
    CmdState *s = cmd_state(cb); if (!s) return;
    s->rec = 1; s->first = s->last = s->open = s->ev_first = s->ev_last = NONE; s->dump_pending = 0; s->ndumps = 0;
    s->ggen++; s->cgen++;   // bindings carry over between recordings in NVN: note them again in this one
    ev(s, E_BEGIN);
    for (int k = 0; k < 8; k++) if (k != 5 && s->gprog[k]) ev(s, E_BIND, 0, 1u << k, s->gprog[k]);
    if (s->cprog) ev(s, E_BIND, 0, NVN_SHADER_STAGE_COMPUTE_BIT, s->cprog);
    stamp(cb, s, K_BEGIN);
}
static NVNcommandHandle w_nvnCommandBufferEndRecording(NVNcommandBuffer *cb) {
    CmdState *s = g_armed.load(std::memory_order_relaxed) ? cmd_state(cb) : nullptr;
    bool rec = s && s->rec;
    if (rec) { stamp(cb, s, K_END); ev(s, E_END); }
    NVNcommandHandle h = real_nvnCommandBufferEndRecording(cb);
    if (rec) { s->rec = 0; for (int i = 0; i < s->ndumps; i++) g_dump[s->dumps[i]].handle = h; Guard g; handle_put(h, s->first, s->ev_first); }
    return h;
}
static void w_nvnCommandBufferSetRenderTargets(NVNcommandBuffer *cb, int n, const NVNtexture *const *c, const NVNtextureView *const *cv, const NVNtexture *d, const NVNtextureView *dv) {
    CmdState *s = armed_state(cb);
    if (s && (g_cfg.modes & M_DUMP)) { dump_at_boundary(cb, s); s->open_kind = 0; }   // the interval on the previous targets ends here: dump it before they change
    if (s) {
        const NVNtexture *rt[5] = {n > 0 && c ? c[0] : nullptr, n > 1 && c ? c[1] : nullptr, n > 2 && c ? c[2] : nullptr, n > 3 && c ? c[3] : nullptr, d};
        if (memcmp(rt, s->rt, sizeof rt) || s->colors != (uint8_t)(n > 0 ? n : 0)) {   // texture queries only when the targets change
            memcpy(s->rt, rt, sizeof rt);
            const NVNtexture *t0 = rt[0] ? rt[0] : d;
            s->colors = (uint8_t)(n > 0 ? n : 0); s->w = t0 ? (uint16_t)real_nvnTextureGetWidth(t0) : 0; s->h = t0 ? (uint16_t)real_nvnTextureGetHeight(t0) : 0;
            for (int i = 0; i < 4; i++) s->cfmt[i] = rt[i] ? (int16_t)real_nvnTextureGetFormat(rt[i]) : 0;
            s->dfmt = d ? (int16_t)real_nvnTextureGetFormat(d) : 0;
        }
        ev(s, E_TARGETS, 0, (uint32_t)s->w | (uint32_t)s->h << 16, s->colors | (uint32_t)(uint16_t)s->cfmt[0] << 8, (uint64_t)(uint16_t)s->dfmt);
    }
    real_nvnCommandBufferSetRenderTargets(cb, n, c, cv, d, dv);
    if (s) stamp(cb, s, K_R);
}
static void w_nvnCommandBufferBindProgram(NVNcommandBuffer *cb, const NVNprogram *p, int stages) {
    real_nvnCommandBufferBindProgram(cb, p, stages);
    CmdState *s = armed_state(cb); if (!s) return;
    uint32_t k = prog_find(p);
    if (stages & NVN_SHADER_STAGE_COMPUTE_BIT) { s->cprog = k; s->cgen++; }
    if (stages & ~NVN_SHADER_STAGE_COMPUTE_BIT & 0xff) { for (int i = 0; i < 8; i++) if (i != 5 && (stages >> i & 1)) s->gprog[i] = k; s->ggen++; }
    ev(s, E_BIND, 0, (uint32_t)stages, k);
}
static void w_nvnCommandBufferBarrier(NVNcommandBuffer *cb, int bits) {
    CmdState *s = armed_state(cb);
    if (s) { Mark *m = open_mark(s); if (m) m->barriers++; ev(s, E_BARRIER, 0, (uint32_t)bits); }
    real_nvnCommandBufferBarrier(cb, bits);
}
// a dispatch or a mesh / task draw: an interval of its own kind (split=run: joins an open one of the same kind)
static inline Mark *split_to(NVNcommandBuffer *cb, CmdState *s, uint8_t kind) {
    if (g_cfg.each || s->open == NONE || s->open_kind != kind) stamp(cb, s, kind);
    return open_mark(s);
}
static void w_nvnCommandBufferDispatchCompute(NVNcommandBuffer *cb, int x, int y, int z) {
    CmdState *s = armed_state(cb);
    if (s) {
        Mark *m = split_to(cb, s, K_C);
        if (m) { if (!m->dispatches) { m->gx = (uint32_t)x; m->gy = (uint32_t)y; m->gz = (uint32_t)z; } m->groups += (uint32_t)x * (uint32_t)y * (uint32_t)z; m->dispatches++; }
        note_use(s, U_COMPUTE); ev(s, E_DISPATCH, 0, (uint32_t)x, (uint32_t)y, (uint32_t)z);
    }
    real_nvnCommandBufferDispatchCompute(cb, x, y, z);
}
static void w_nvnCommandBufferDispatchComputeIndirect(NVNcommandBuffer *cb, NVNbufferAddress a) {
    CmdState *s = armed_state(cb);
    if (s) { Mark *m = split_to(cb, s, K_C); if (m) { m->dispatches++; m->indirect = 1; } note_use(s, U_COMPUTE); ev(s, E_DISPATCH, 1, 0, 0, a); }
    real_nvnCommandBufferDispatchComputeIndirect(cb, a);
}
static void w_nvnCommandBufferDrawMeshTasks(NVNcommandBuffer *cb, int first, int count) {
    CmdState *s = armed_state(cb);
    if (s) { Mark *m = split_to(cb, s, K_M); if (m) { m->draws++; m->groups += (uint32_t)count; } s->open_draws++; note_use(s, U_MESH); ev(s, E_MESH, 0, (uint32_t)count); }
    real_nvnCommandBufferDrawMeshTasks(cb, first, count);
}
static void w_nvnCommandBufferDrawMeshTasksIndirect(NVNcommandBuffer *cb, NVNbufferAddress a) {
    CmdState *s = armed_state(cb);
    if (s) { Mark *m = split_to(cb, s, K_M); if (m) { m->draws++; m->indirect = 1; } s->open_draws++; note_use(s, U_MESH); ev(s, E_MESH, 1); }
    real_nvnCommandBufferDrawMeshTasksIndirect(cb, a);
}
// an ordinary draw: back to a render interval if a dispatch or mesh run is open (the render boundary is taken here, lazily), count it, note a changed program
static inline void count_draw(NVNcommandBuffer *cb, uint8_t indirect) {
    CmdState *s = armed_state(cb); if (!s) return;
    if (s->open == NONE || s->open_kind == K_C || s->open_kind == K_M) stamp(cb, s, K_R);
    Mark *m = open_mark(s); if (m) m->draws++;
    s->open_draws++; note_use(s, U_DRAW); ev(s, E_DRAW, indirect);
}
static void w_nvnCommandBufferDrawArrays(NVNcommandBuffer *cb, int pr, int f, int c) { count_draw(cb, 0); real_nvnCommandBufferDrawArrays(cb, pr, f, c); }
static void w_nvnCommandBufferDrawElements(NVNcommandBuffer *cb, int pr, int it, int c, NVNbufferAddress a) { count_draw(cb, 0); real_nvnCommandBufferDrawElements(cb, pr, it, c, a); }
static void w_nvnCommandBufferDrawElementsBaseVertex(NVNcommandBuffer *cb, int pr, int it, int c, NVNbufferAddress a, int bv) { count_draw(cb, 0); real_nvnCommandBufferDrawElementsBaseVertex(cb, pr, it, c, a, bv); }
static void w_nvnCommandBufferDrawArraysInstanced(NVNcommandBuffer *cb, int pr, int f, int c, int bi, int ic) { count_draw(cb, 0); real_nvnCommandBufferDrawArraysInstanced(cb, pr, f, c, bi, ic); }
static void w_nvnCommandBufferDrawElementsInstanced(NVNcommandBuffer *cb, int pr, int it, int c, NVNbufferAddress a, int bv, int bi, int ic) { count_draw(cb, 0); real_nvnCommandBufferDrawElementsInstanced(cb, pr, it, c, a, bv, bi, ic); }
static void w_nvnCommandBufferDrawArraysIndirect(NVNcommandBuffer *cb, int pr, NVNbufferAddress a) { count_draw(cb, 1); real_nvnCommandBufferDrawArraysIndirect(cb, pr, a); }
static void w_nvnCommandBufferDrawElementsIndirect(NVNcommandBuffer *cb, int pr, int it, NVNbufferAddress ib, NVNbufferAddress a) { count_draw(cb, 1); real_nvnCommandBufferDrawElementsIndirect(cb, pr, it, ib, a); }
static void w_nvnCommandBufferMultiDrawArraysIndirectCount(NVNcommandBuffer *cb, int pr, NVNbufferAddress a, NVNbufferAddress n, int mx, ptrdiff_t st) { count_draw(cb, 1); real_nvnCommandBufferMultiDrawArraysIndirectCount(cb, pr, a, n, mx, st); }
static void w_nvnCommandBufferMultiDrawElementsIndirectCount(NVNcommandBuffer *cb, int pr, int it, NVNbufferAddress ib, NVNbufferAddress a, NVNbufferAddress n, int mx, ptrdiff_t st) { count_draw(cb, 1); real_nvnCommandBufferMultiDrawElementsIndirectCount(cb, pr, it, ib, a, n, mx, st); }
static void w_nvnCommandBufferWaitSync(NVNcommandBuffer *cb, const NVNsync *y) {
    CmdState *s = armed_state(cb); if (s) { Mark *m = open_mark(s); if (m) m->waits++; ev(s, E_WAIT_SYNC, 0, 0, 0, (uint64_t)(uintptr_t)y); }
    real_nvnCommandBufferWaitSync(cb, y);
}
static void w_nvnCommandBufferFenceSync(NVNcommandBuffer *cb, NVNsync *y, int c, int f) {
    CmdState *s = armed_state(cb); if (s) { Mark *m = open_mark(s); if (m) m->signals++; ev(s, E_FENCE_SYNC, 0, (uint32_t)c, (uint32_t)f, (uint64_t)(uintptr_t)y); }
    real_nvnCommandBufferFenceSync(cb, y, c, f);
}
static void w_nvnCommandBufferWaitEvent(NVNcommandBuffer *cb, const NVNevent *e, int mode, uint32_t v) {
    CmdState *s = armed_state(cb); if (s) { Mark *m = open_mark(s); if (m) m->waits++; ev(s, E_WAIT_EVENT, (uint8_t)mode, 0, v, (uint64_t)(uintptr_t)e); }
    real_nvnCommandBufferWaitEvent(cb, e, mode, v);
}
static void w_nvnCommandBufferSignalEvent(NVNcommandBuffer *cb, const NVNevent *e, int mode, int loc, int fl, uint32_t v) {
    CmdState *s = armed_state(cb); if (s) { Mark *m = open_mark(s); if (m) m->signals++; ev(s, E_SIGNAL_EVENT, (uint8_t)mode, (uint32_t)(loc & 0xff) | (uint32_t)fl << 8, v, (uint64_t)(uintptr_t)e); }
    real_nvnCommandBufferSignalEvent(cb, e, mode, loc, fl, v);
}
static void w_nvnCommandBufferReportCounter(NVNcommandBuffer *cb, int type, NVNbufferAddress a) {   // the game's own reports (the module's stamps call the real function)
    CmdState *s = armed_state(cb);
    if (s) {
        ev(s, E_REPORT, 0, (uint32_t)type, 0, a);
        if (logging_now()) {
            g_gctr[type >= 0 && type < NCTYPE - 1 ? type : NCTYPE - 1].fetch_add(1, std::memory_order_relaxed);
            if (type == NVN_COUNTER_TYPE_TIMESTAMP || type == NVN_COUNTER_TYPE_TIMESTAMP_TOP) {
                Guard g; int i = 0; for (; i < g_ngts && !(g_gts[i].addr == a && g_gts[i].type == type); i++) {}
                if (i < g_ngts) g_gts[i].n++; else if (g_ngts < MAXGTS) g_gts[g_ngts++] = {a, type, 1};
            }
        }
    }
    real_nvnCommandBufferReportCounter(cb, type, a);
}
static void w_nvnCommandBufferSetRenderEnable(NVNcommandBuffer *cb, uint32_t on) { CmdState *s = armed_state(cb); if (s) ev(s, E_RENDER_ENABLE, 0, on); real_nvnCommandBufferSetRenderEnable(cb, on); }
static void w_nvnCommandBufferSetRenderEnableConditional(NVNcommandBuffer *cb, int mode, NVNbufferAddress a) { CmdState *s = armed_state(cb); if (s) ev(s, E_RENDER_COND, 0, (uint32_t)mode, 0, a); real_nvnCommandBufferSetRenderEnableConditional(cb, mode, a); }
static void w_nvnCommandBufferClearColor(NVNcommandBuffer *cb, int i, const float *c, int mask) { CmdState *s = armed_state(cb); if (s) ev(s, E_CLEAR_COLOR, 0, (uint32_t)i, (uint32_t)mask); real_nvnCommandBufferClearColor(cb, i, c, mask); }
static void w_nvnCommandBufferClearDepthStencil(NVNcommandBuffer *cb, float d, uint32_t dm, int sv, int sm) { CmdState *s = armed_state(cb); if (s) ev(s, E_CLEAR_DS, 0, dm, (uint32_t)sm); real_nvnCommandBufferClearDepthStencil(cb, d, dm, sv, sm); }
static void w_nvnCommandBufferCopyBufferToBuffer(NVNcommandBuffer *cb, NVNbufferAddress a, NVNbufferAddress b, size_t n, int f) { CmdState *s = armed_state(cb); if (s) ev(s, E_COPY_B2B, 0, (uint32_t)n); real_nvnCommandBufferCopyBufferToBuffer(cb, a, b, n, f); }
static void w_nvnCommandBufferCopyBufferToTexture(NVNcommandBuffer *cb, NVNbufferAddress a, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, int f) { CmdState *s = armed_state(cb); if (s) ev(s, E_COPY_B2T, 0, r ? (uint32_t)r->width : 0, r ? (uint32_t)r->height : 0); real_nvnCommandBufferCopyBufferToTexture(cb, a, t, v, r, f); }
static void w_nvnCommandBufferCopyTextureToBuffer(NVNcommandBuffer *cb, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, NVNbufferAddress a, int f) { CmdState *s = armed_state(cb); if (s) ev(s, E_COPY_T2B, 0, r ? (uint32_t)r->width : 0, r ? (uint32_t)r->height : 0); real_nvnCommandBufferCopyTextureToBuffer(cb, t, v, r, a, f); }
static void w_nvnCommandBufferCopyTextureToTexture(NVNcommandBuffer *cb, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const NVNtexture *t2, const NVNtextureView *v2, const NVNcopyRegion *r2, int f) { CmdState *s = armed_state(cb); if (s) ev(s, E_COPY_T2T, 0, r ? (uint32_t)r->width : 0, r ? (uint32_t)r->height : 0); real_nvnCommandBufferCopyTextureToTexture(cb, t, v, r, t2, v2, r2, f); }
static void w_nvnCommandBufferSetCopyRowStride(NVNcommandBuffer *cb, ptrdiff_t v) { CmdState *s = cmd_state(cb); if (s) s->row_stride = v; real_nvnCommandBufferSetCopyRowStride(cb, v); }   // kept always: a dump restores the game's value
static void w_nvnCommandBufferSetCopyImageStride(NVNcommandBuffer *cb, ptrdiff_t v) { CmdState *s = cmd_state(cb); if (s) s->image_stride = v; real_nvnCommandBufferSetCopyImageStride(cb, v); }

static void w_nvnQueueSubmitCommands(NVNqueue *q, int n, const NVNcommandHandle *h) {
    if (logging_now()) {
        Guard g; uint16_t qi = (uint16_t)queue_id(q);
        for (int i = 0; i < n && g_nsub < MAXSUB; i++) {
            HandleRange *r = handle_get(h[i]);
            SubRange s = {g_present, qi, 0, (uint8_t)!r, r ? r->first : NONE, r ? r->ev_first : NONE, h[i]};
            if (r && ++r->submits > 1) { s.dup = 1; for (int k = 0; k < g_nsub; k++) if (g_sub[k].h == h[i]) g_sub[k].dup = 1; }   // one set of stamps for all its runs: the last one's
            g_sub[g_nsub++] = s;
            if (r && (g_cfg.modes & M_DUMP)) { uint32_t nd = g_ndump.load(); if (nd > MAXDUMP) nd = MAXDUMP; for (uint32_t d = 0; d < nd; d++) if (g_dump[d].handle == h[i]) g_dump[d].frame = g_present; }
        }
    }
    real_nvnQueueSubmitCommands(q, n, h);
}
static void queue_ev(NVNqueue *q, char what, const void *obj) { if (logging_now()) { Guard g; if (g_nqev < MAXQEV) g_qev[g_nqev++] = {g_present, (uint32_t)g_nsub, (uint16_t)queue_id(q), what, obj}; } }
static NVNboolean w_nvnQueueWaitSync(NVNqueue *q, const NVNsync *s) { queue_ev(q, 'W', s); return real_nvnQueueWaitSync(q, s); }
static void w_nvnQueueFenceSync(NVNqueue *q, NVNsync *s, int c, int f) { queue_ev(q, 'F', s); real_nvnQueueFenceSync(q, s, c, f); }
static void w_nvnQueuePresentTexture(NVNqueue *q, NVNwindow *w, int index) {
    queue_ev(q, 'P', w);
    real_nvnQueuePresentTexture(q, w, index);
    long frame = g_present++;
    if (g_state == ST_ARMED && frame == g_log_to) {
        // the last logged frame: stop recording and fence every queue the capture used. No flush and no wait: the game's next present flushes the fences
        g_armed.store(false);
        Guard g; queue_id(q);
        for (int i = 0; i < g_nqueue; i++) { g_fenced[i] = g_sync_ok[i]; if (g_fenced[i]) real_nvnQueueFenceSync(g_queues[i], &g_sync[i], NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0); }
        g_state = ST_DRAIN; g_drain_from = frame;
        return;
    }
    if (g_state == ST_DRAIN) {   // poll, never wait
        bool done = true;
        for (int i = 0; i < g_nqueue && done; i++) if (g_fenced[i]) { int r = real_nvnSyncWait(&g_sync[i], 0); done = r == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || r == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED; }
        bool timed_out = !done && g_present - g_drain_from > 600;
        if (done || timed_out) {
            if (timed_out) st_log("switch_trace: capture %d: the GPU did not finish within 600 frames; writing what there is", g_capture_no);
            uint64_t t0 = st_cpu_ticks();
            { Guard g; if (g_cfg.modes & M_TIMING) write_timing(); if (g_cfg.modes & M_SEQ) write_seq(); if (g_cfg.modes & M_DUMP) write_dumps(); }
            st_log("switch_trace: capture %d written in %.1f ms", g_capture_no, (st_cpu_ticks() - t0) * 1e3 / (double)st_cpu_tick_hz());
            g_state = ST_IDLE; g_log_from = g_log_to = -1;
        }
        return;
    }
    if (g_state == ST_IDLE) {   // triggers: the frame number, the combo (on release, so the press is over before the capture), the go file (checked once a second)
        uint32_t held = g_cfg.combo ? st_buttons() & g_cfg.combo : 0;
        bool combo = g_cfg.combo && held == g_cfg.combo;
        if (g_cfg.start_frame >= 0 && g_present <= g_cfg.start_frame - 2 && g_present + g_cfg.delay >= g_cfg.start_frame) trigger(g_cfg.start_frame);
        else if (g_combo_was && !combo) trigger(g_present + g_cfg.delay);
        else if (g_present % 60 == 0) { char p[160]; snprintf(p, sizeof p, "%s/go", DIR); if (st_fs_exists(p)) { st_fs_delete(p); trigger(g_present + g_cfg.delay); } }
        g_combo_was = combo;
    }
    if (g_state == ST_PENDING && g_present >= g_log_from - 2) arm();   // g_present: the frame now being built
}

// ---------------------------------------------------------------- the name table: what the game is handed
struct Wrap { const char *name; PFN wrapper; PFN *real; };
#define W(n) {#n, (PFN)w_##n, (PFN *)&real_##n}
#define ONLY_REAL(n) {#n, nullptr, (PFN *)&real_##n}   // the module calls it; the game gets the real one
static Wrap g_wraps[] = {
    W(nvnDeviceInitialize), W(nvnMemoryPoolInitialize), W(nvnMemoryPoolFinalize),
    W(nvnProgramSetShaders), W(nvnProgramSetShadersExt), W(nvnProgramBuilderSetShaders), W(nvnProgramInitializeFromBuilder),
    W(nvnCommandBufferBeginRecording), W(nvnCommandBufferEndRecording), W(nvnCommandBufferSetRenderTargets), W(nvnCommandBufferBindProgram), W(nvnCommandBufferBarrier),
    W(nvnCommandBufferDispatchCompute), W(nvnCommandBufferDispatchComputeIndirect), W(nvnCommandBufferDrawMeshTasks), W(nvnCommandBufferDrawMeshTasksIndirect),
    W(nvnCommandBufferDrawArrays), W(nvnCommandBufferDrawElements), W(nvnCommandBufferDrawElementsBaseVertex), W(nvnCommandBufferDrawArraysInstanced),
    W(nvnCommandBufferDrawElementsInstanced), W(nvnCommandBufferDrawArraysIndirect), W(nvnCommandBufferDrawElementsIndirect),
    W(nvnCommandBufferMultiDrawArraysIndirectCount), W(nvnCommandBufferMultiDrawElementsIndirectCount),
    W(nvnCommandBufferWaitSync), W(nvnCommandBufferWaitEvent), W(nvnCommandBufferFenceSync), W(nvnCommandBufferSignalEvent), W(nvnCommandBufferReportCounter),
    W(nvnCommandBufferSetRenderEnable), W(nvnCommandBufferSetRenderEnableConditional), W(nvnCommandBufferClearColor), W(nvnCommandBufferClearDepthStencil),
    W(nvnCommandBufferCopyBufferToBuffer), W(nvnCommandBufferCopyBufferToTexture), W(nvnCommandBufferCopyTextureToBuffer), W(nvnCommandBufferCopyTextureToTexture),
    W(nvnCommandBufferSetCopyRowStride), W(nvnCommandBufferSetCopyImageStride),
    W(nvnQueueSubmitCommands), W(nvnQueueWaitSync), W(nvnQueueFenceSync), W(nvnQueuePresentTexture),
    ONLY_REAL(nvnDeviceGetTimestampInNanoseconds), ONLY_REAL(nvnMemoryPoolBuilderSetDefaults), ONLY_REAL(nvnMemoryPoolBuilderSetDevice),
    ONLY_REAL(nvnMemoryPoolBuilderSetStorage), ONLY_REAL(nvnMemoryPoolBuilderSetFlags), ONLY_REAL(nvnMemoryPoolMap), ONLY_REAL(nvnMemoryPoolGetBufferAddress),
    ONLY_REAL(nvnMemoryPoolGetSize), ONLY_REAL(nvnMemoryPoolGetFlags), ONLY_REAL(nvnTextureGetWidth), ONLY_REAL(nvnTextureGetHeight), ONLY_REAL(nvnTextureGetFormat),
    ONLY_REAL(nvnSyncInitialize), ONLY_REAL(nvnSyncWait),
};
static PFN resolve(const char *name, PFN real) {
    for (Wrap &w : g_wraps) if (!strcmp(w.name, name)) { if (real) *w.real = real; return w.wrapper && real ? w.wrapper : real; }
    return real;
}
static void fill_reals(void) {   // the functions the module calls itself, whether or not the game asks for them (NVN resolves most names without a device)
    static bool done; if (done || !real_gpa) return; done = true;
    for (Wrap &w : g_wraps) if (!*w.real) *w.real = real_gpa(nullptr, w.name);
}
static PFN w_nvnDeviceGetProcAddress(const NVNdevice *d, const char *name) { fill_reals(); return resolve(name, real_gpa(d, name)); }
static void *w_nvnBootstrapLoader(const char *name) {
    void *p = real_bootstrap(name);
    if (!strcmp(name, "nvnDeviceGetProcAddress")) { real_gpa = (PFN (*)(const NVNdevice *, const char *))p; fill_reals(); return p ? (void *)w_nvnDeviceGetProcAddress : nullptr; }
    if (real_gpa) fill_reals();
    return (void *)resolve(name, (PFN)p);
}

// ---------------------------------------------------------------- entry: the mod framework calls this once, before the game initializes NVN
extern "C" void switch_trace_main(void) {
    st_fs_mount(); load_config();
    g_stage = (Stage *)calloc(MAXSTAGE, sizeof(Stage));
    if (!g_stage) st_log("switch_trace: no memory for the stage table (%zu bytes): no shader keys", MAXSTAGE * sizeof(Stage));
    real_bootstrap = st_hook_bootstrap(w_nvnBootstrapLoader);
    char combo[96] = ""; size_t k = 0;
    for (int i = 0; i < 16; i++) if (g_cfg.combo >> i & 1) k += (size_t)snprintf(combo + k, sizeof combo - k, "%s%s", k ? "+" : "", BUTTON_NAMES[i]);
    st_log("switch_trace: loaded in %s (mode=%s, start_frame %ld, frames %ld, delay %ld, combo %s, split=%s, key_skip %#x:%u, ext_stride %#x, dump keys %d, dump_all %d)", DIR, modes_str(),
           g_cfg.start_frame, g_cfg.frames, g_cfg.delay, g_cfg.combo ? combo : "off", g_cfg.each ? "each" : "run", g_cfg.key_skip_off, g_cfg.key_skip_len, g_cfg.ext_stride,
           g_cfg.ndump_keys, g_cfg.dump_all);
}
