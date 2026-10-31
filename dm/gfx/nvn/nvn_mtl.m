// Metal execution layer of the NVN null device, see nvn_mtl.h.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#import <MetalFX/MetalFX.h>
#undef I   // MetalFX brings in <complex.h>, whose I (the imaginary unit) would shadow the local tables named I below
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>   // os_proc_available_memory: for the memory readout line
#endif
#include "nvn_mtl.h"
#include "nvn_obj.h"
#include "../tex/nvn_fmt_mtl.h"
#include "../tex/nvn_deswizzle.h"
#include "../tex/nvn_deswizzle_gpu.h"
#include "nvn_builtin.h"
#include "nvn_dirty.h"
#include "nvn_depth_clear.h"
#include "nvn_refl.h"
#include "nvn_cover.h"
#include "nvn_border.h"
#include "nvn_primitive.h"
#include "nvn_timestamp.h"
#include "dmsh_chain.h"   // DMSH_CHAIN: the translation chain version of the B shader library expected by this source tree, one for every game (beside this file, tools/dmsh_chain.py --header)
#include "game_profile.h"   // game profile constants (profile id, the profile's switches)
// Texture slot table per stage: 0-127 the combined handles (nvnCommandBufferBindTexture), 128-255 the separate texture binds (NVN_SEPTEX_SLOT0, nvn_obj.h)
#define NTEX 256
// The NVN driver's constant segments c0 and c2 (byte offsets, slot counts) and the binding kinds of the shader reflection have one definition, which the shader build reads too (dmsh_build.py through dmsh_abi.py)
#include "dmsh_abi.h"
// c2 holds the handle tables (DMSH_C2_*), c0 the storage buffer table of each stage at DMSH_C0_SSBO_<stage> + 0x10 r (address 64 bits, size 32 bits). The replay fills one DMSH_DRV_SIZE buffer per segment a shader reads
_Static_assert(DMSH_C2_TEX + DMSH_C2_ROW * DMSH_C2_TEX_SLOTS == DMSH_C2_SEPTEX && DMSH_C2_SEPTEX + DMSH_C2_ROW * DMSH_C2_SEP_SLOTS == DMSH_C2_SAMP && DMSH_C2_SAMP + DMSH_C2_ROW * DMSH_C2_SEP_SLOTS <= DMSH_DRV_SIZE && NVN_SEPTEX_SLOT0 + DMSH_C2_SEP_SLOTS == NTEX, "NVN driver handle tables");
_Static_assert(DMSH_C0_CS_UBO + DMSH_C0_ROW * DMSH_C0_CS_UBO_SLOTS == DMSH_C0_SSBO_CS && DMSH_C0_SSBO_PS + DMSH_C0_ROW * DMSH_C0_SSBO_SLOTS <= DMSH_C0_POINT_SIZE && DMSH_C0_POINT_SIZE + 4 <= DMSH_C0_SAMPLE_TABLE &&
               DMSH_C0_SAMPLE_TABLE + DMSH_C0_SAMPLE_BYTES <= DMSH_C0_SAMPLE_INFO && DMSH_C0_SAMPLE_INFO + 16 <= DMSH_C0_BASE_INSTANCE && DMSH_C0_BASE_INSTANCE + 4 <= DMSH_DRV_SIZE, "NVN driver c0 tables");
// Geometry's counter/output addresses are c0+0x410/0x430 (NieR:Automata's MOVC/IADDC pairs); they use the logical NVN stage even when Metal executes a compute kernel.
static int c0_ssbo_base(int stage) { return stage == 0 ? DMSH_C0_SSBO_VS : stage == 1 ? DMSH_C0_SSBO_PS : stage == 2 ? DMSH_C0_SSBO_GS : stage == 5 ? DMSH_C0_SSBO_CS : -1; }   // NVN stages 0 vertex, 1 pixel, 2 geometry, 5 compute; hull / domain never seen
// The handle a texture binding of slot reg reads: as c2 holds it (encode_args fills c2 the same way), a combined slot past 31 shares its word with an image slot, and a bound image
// is what lies there. A texture instruction on an image handle names such a slot: a size query of the image a compute shader writes (Unreal's GetDimensions on a RWTexture3D,
// dmsh_build.py decode_tex). DRAGON QUEST VII Reimagined's volumetric lightmap indirection patch read the empty texture slot 32 there, got a 1x1 size and wrote only cell (0, 0, 0)
static inline uint64_t tex_slot_h(const uint64_t *tex, const uint64_t *img, unsigned reg) {
    return reg >= 32 && reg - 32 < DMSH_C2_IMG_SLOTS && img[reg - 32] ? img[reg - 32] : reg < NTEX ? tex[reg] : 0;
}
static uint64_t c2_handle(int stage, unsigned b);   // the handle c2 holds at byte b (defined with the draw state, TEXA / SAMPA)
#include "nvn_hmap.h"
#include "../../host/dm.h"
#import <CommonCrypto/CommonDigest.h>
#include <stdarg.h>
#include <mach-o/dyld.h>
#include <mach-o/fat.h>
#include <libkern/OSByteOrder.h>
#include <libgen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include <errno.h>
#include <execinfo.h>
#include <os/lock.h>
#include <objc/runtime.h>
// Function parameters in this file are never retained again in the callee (the caller / global tables already hold them, and lifetime is guaranteed by defer_obj_release): saves a retain / release pair per call on the hot path
#pragma clang attribute push(__attribute__((objc_externally_retained)), apply_to = function)

#define MLOG(...) do { flockfile(stderr); fprintf(stderr, "[dm] [nvn-mtl] " __VA_ARGS__); fputc('\n', stderr); funlockfile(stderr); } while (0)   // one line under one lock, as LOG (dm.h)
#define MDIE(...) do { MLOG("FATAL " __VA_ARGS__); abort(); } while (0)

static void warm_start(void);
static void shmap_load(void);
static int prepare_on(void);
static int g_warm_started;
static void warm_claim(void);

static id<MTLDevice> g_dev;
static NSMutableArray *g_l3_gpu_leaks;
static int g_nvn_deswz_gpu;
static id<MTLCommandQueue> g_q;
static id<MTL4CommandQueue> g_q4;
static int g_classic_ok, g_classic;   // mtl_device_classic was asked / the device has no Metal 4 and only the classic part started (mtl_init)
static dispatch_queue_t g_feedback_q;
static _Atomic uint32_t g_present_qos;
#define ALLOC_MAX 8
static id<MTL4CommandAllocator> g_alloc4[ALLOC_MAX]; static int g_alloc_n, g_alloc_cur;   // Command allocator pool of the main submission: at start pick one the GPU has finished with, create a new one if there is none (wait only at the cap)
static id<MTL4CommandAllocator> g_up_alloc4[3];
static id<MTL4CommandAllocator> g_present_alloc4[3];
static id<MTL4CommandBuffer> g_present_cb[3];
static id<MTL4ArgumentTable> g_vt, g_ft, g_ct, g_up_ct;
static int g_vbuf_cache = 1;   // DM_NVN_VBUF_CACHE=0 restores the uncached path for A/B measurements
static uint64_t g_vt_addr[31]; static uint32_t g_vt_valid;
static uint64_t g_vbuf_resolve, g_vbuf_reuse, g_vbuf_set, g_vbuf_skip, g_vbuf_copies, g_vbuf_copy_bytes;   // copies: vertex streams at addresses not 4-byte aligned (vbuf_unaligned)
static id<MTL4ArgumentTable> g_present_args;
static id<MTLBuffer> g_present_uv[3];
static uint64_t g_present_alloc_fence[3];
// Under g_pq_mu: per present allocator, the newest ticket its commit feedback reported done on the GPU. present_now resets an allocator only once this reaches the ticket that
// last used it. g_pq_done is not that: every display-link callback sets it to ticket - 1 (the previous present was committed, not run) and a present that was not submitted sets
// it to its own ticket. Waiting on g_pq_done, a display-link present never waited here: with three presents in flight, or two after a dropped frame, the allocator was reset
// under a present the GPU had not run, and that present's drawable read back black (t_present_alloc; docs/reference/frame-rate.md)
static uint64_t g_present_alloc_done[3];
static pthread_mutex_t g_pq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pq_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_link_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_link_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_link_tick_ns, g_link_tick_n;
static uint64_t g_link_dropped;
static uint64_t g_link_empty;   // Under g_pq_mu: ticks that found no new frame queued (counted here, not as ticks minus presents: presents before the link starts and while it is rescued are not on ticks)
static double g_link_hold_until; static uint64_t g_link_held;   // Changed only on the display-link callback thread: the time the previous frame must stay on screen until by its present interval (0 = no hold), and the ticks that took no new frame because of it
static int g_pq_inflight;   // Under g_pq_mu: number of presents committed to the GPU whose completion callback has not arrived yet
int g_dm_present_link;   // Present pacing mode (runtime value of DM_PRESENT_LINK, modified directly during same-process alternating measurement): 0 = acquire the drawable itself; 1 = display link at 60 Hz; 2 = display link at 120 Hz, the game presents a frame every other display-link tick
static int g_present_latency;   // DM_PRESENT_LATENCY: the display link's frame latency in a window and in fullscreen alike (0 = not set: link_latency picks it)
static uint64_t g_pq_done;
static uint64_t g_pq_read;
static id<MTLResidencySet> g_res;
static int g_res_dirty;
static id<MTLSharedEvent> g_ev;
static _Atomic uint64_t g_fence;                // Highest committed fence value (incremented exclusively by the submit thread)
static int g_cut_copy;                          // a buffer → texture copy was encoded since the last commit: the next pass boundary commits the segment (sub_cut)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_done;                 // GPU complete and all CPU-visible effects through this fence published
static uint64_t g_arena_made;
static int g_allocprof = -1; static uint64_t g_ap_bytes[64], g_ap_fn[8];
// Diagnostic only: inclusive wall/CPU time on the replay thread. No GPU waits
// are added, and no clocks are read when DM_NVN_CPUPROF is absent or zero.
typedef struct { uint64_t wall, cpu; } cp_stamp;
static _Thread_local uint64_t g_cp_wall[8], g_cp_cpu[8], g_cp_calls[8], g_cp_last;
static _Thread_local uint64_t g_cp_geo_waited, g_cp_geo_safe;
static int cp_on(void) { static _Thread_local int on = -1; if (on < 0) { const char *e = getenv("DM_NVN_CPUPROF"); on = e && atoi(e); } return on; }
static cp_stamp cp_begin(void) {
    if (!cp_on()) return (cp_stamp){0};
    return (cp_stamp){clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW), clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)};
}
static void cp_end(int phase, cp_stamp start) {
    if (!start.wall) return;
    g_cp_cpu[phase] += clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - start.cpu;
    g_cp_wall[phase] += clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - start.wall;
    g_cp_calls[phase]++;
}
static void cp_report(void) {
    if (!cp_on()) return;
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    if (!g_cp_last) { g_cp_last = now; return; }
    if (now - g_cp_last < 5000000000ull) return;
    static const char *names[] = {"pass_end", "pass_begin", "pipeline", "arguments", "texture", "draw", "geometry", "index"};
    char line[1000] = {0}; size_t at = 0;
    for (int i = 0; i < 8; i++) if (g_cp_calls[i])
        at += snprintf(line + at, sizeof line - at, " %s %llu calls %.2f/%.2f ms CPU/wall;", names[i], (unsigned long long)g_cp_calls[i], g_cp_cpu[i] / 1e6, g_cp_wall[i] / 1e6);
    MLOG("[cpuprof] inclusive phases over %.2f s (nested phases overlap):%s", (now - g_cp_last) / 1e9, line);
    if (g_cp_geo_waited || g_cp_geo_safe) MLOG("[cpuprof] geometry CPU reads: waited %llu / proven safe %llu", (unsigned long long)g_cp_geo_waited, (unsigned long long)g_cp_geo_safe);
    memset(g_cp_wall, 0, sizeof g_cp_wall); memset(g_cp_cpu, 0, sizeof g_cp_cpu); memset(g_cp_calls, 0, sizeof g_cp_calls); g_cp_last = now;
    g_cp_geo_waited = g_cp_geo_safe = 0;
}
#define AP_B uint64_t _ap0 = g_allocprof > 0 ? g_dev.currentAllocatedSize : 0; cp_stamp _cp0 = cp_begin()
#define AP_E(i) do { if (g_allocprof > 0) { uint64_t _n = g_dev.currentAllocatedSize; if (_n > _ap0) g_ap_fn[i] += _n - _ap0; } cp_end(i, _cp0); } while (0)   // Per-frame arenas created so far (4 MiB each; for the memory report)
static _Atomic int g_last_commit_error;
static _Atomic int g_up_pending;

static pthread_mutex_t g_res_mu = PTHREAD_MUTEX_INITIALIZER;   // Dedicated lock for the residency set: res_add is called on paths that hold g_mu (arena_new), so it cannot share g_mu
// Wait ledger (DM_NVN_WAITS=1): every place a thread of the host or the game waits on the CPU for the GPU or for the encode thread, counted per frame (calls, calls that blocked,
// the time they blocked) and logged every 300 frames with the frame's GPU idle. The game's own SyncWait is split by how far its fence is behind the newest one (0: the last submission)
enum { WS_SYNC, WS_SYNC_NEWEST, WS_SYNC_POLL, WS_ACQ, WS_FINISH, WS_ALLOC, WS_VIS, WS_PG, WS_MDI, WS_READTEX, WS_T2T, WS_DIAG, WS_SNAP, WS_UPZERO, WS_UPALLOC, WS_DRAIN, WS_UBC, WS_OLDQ, WS_N };
static const char *const ws_name[WS_N] = {"SyncWait (game, older fence)", "SyncWait (game, newest fence)", "SyncWait polls", "SyncWait on an acquire sync", "QueueFinish", "allocator", "occlusion ring", "PASSGPU ring",
    "indirect-count multi-draw", "texture readback", "texture copy t2t", "diagnostics", "snapshot ring", "upload batches", "upload slot", "encode thread drain", "uniform copy worker", "old-queue command buffer"};
static _Atomic uint64_t g_ws_n[WS_N], g_ws_blk[WS_N], g_ws_ns[WS_N];
static int ws_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_WAITS"); v = e && atoi(e) > 0; } return v; }
static inline uint64_t ws_t0(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static inline void ws_put(int site, uint64_t t0, int blocked) {
    atomic_fetch_add_explicit(&g_ws_n[site], 1, memory_order_relaxed);
    if (blocked) { atomic_fetch_add_explicit(&g_ws_blk[site], 1, memory_order_relaxed); atomic_fetch_add_explicit(&g_ws_ns[site], ws_t0() - t0, memory_order_relaxed); }
}
// For the game's waits: when each fence's command buffer ended on the GPU and when its feedback handler ran (CACurrentMediaTime), so a wait that blocked tells its wake-up delay
static struct { _Atomic uint64_t fence, wfence; double gpu_end, fb, wake; _Atomic int done; } g_fend[64];
static _Atomic uint64_t g_ws_wake_n, g_ws_wake_ns, g_ws_fb_ns;
static void ws_wake_pair(unsigned k) {   // both halves known: the wake-up delay after the GPU end, counted once
    if (atomic_load(&g_fend[k].fence) != atomic_load(&g_fend[k].wfence) || atomic_exchange(&g_fend[k].done, 1)) return;
    atomic_fetch_add(&g_ws_wake_n, 1); atomic_fetch_add(&g_ws_wake_ns, (uint64_t)(fmax(0, g_fend[k].wake - g_fend[k].gpu_end) * 1e9)); atomic_fetch_add(&g_ws_fb_ns, (uint64_t)(fmax(0, g_fend[k].fb - g_fend[k].gpu_end) * 1e9));
}
static void ws_wake(uint64_t fence) {   // a blocking SyncWait on this fence has woken
    unsigned k = fence % 64; g_fend[k].wake = CACurrentMediaTime(); atomic_store(&g_fend[k].wfence, fence); ws_wake_pair(k);
    static int once; if (!once++) { char nm[64] = ""; pthread_getname_np(pthread_self(), nm, sizeof nm); MLOG("[waits] first blocking SyncWait on the newest fence: thread '%s'", nm); }
}
static void ws_fb(uint64_t fence, double gpu_end) {   // the fence's feedback handler
    unsigned k = fence % 64; if (atomic_load(&g_fend[k].fence) != fence) { atomic_store(&g_fend[k].done, 0); g_fend[k].gpu_end = gpu_end; g_fend[k].fb = CACurrentMediaTime(); atomic_store(&g_fend[k].fence, fence); }
    ws_wake_pair(k);
}
static _Atomic uint64_t g_ws_post[5];
static void ws_report(uint64_t frame, uint64_t frames) {
    char line[2048]; size_t o = 0;
    { uint64_t e = atomic_exchange(&g_ws_post[1], 0), sp = atomic_exchange(&g_ws_post[2], 0), ts = atomic_exchange(&g_ws_post[3], 0), oc = atomic_exchange(&g_ws_post[4], 0);
      if (e + sp + ts + oc) o += snprintf(line + o, sizeof line - o, " CPU-side writes at GPU end: events %.2f, samples-passed reports %.2f, timestamp reports %.2f, other counter reports %.2f;", (double)e / frames, (double)sp / frames, (double)ts / frames, (double)oc / frames); }
    uint64_t wn = atomic_exchange(&g_ws_wake_n, 0), wns = atomic_exchange(&g_ws_wake_ns, 0), fns = atomic_exchange(&g_ws_fb_ns, 0);
    if (wn) o += snprintf(line + o, sizeof line - o, " newest-fence SyncWait wakes %.3f ms after the GPU end (feedback handler at %.3f ms);", wns / 1e6 / wn, fns / 1e6 / wn);
    for (int i = 0; i < WS_N; i++) { uint64_t n = atomic_exchange(&g_ws_n[i], 0), b = atomic_exchange(&g_ws_blk[i], 0), ns = atomic_exchange(&g_ws_ns[i], 0);
        if (n && o < sizeof line - 160) o += snprintf(line + o, sizeof line - o, " %s %.2f/frame (blocked %.2f, %.3f ms);", ws_name[i], (double)n / frames, (double)b / frames, ns / 1e6 / frames); }
    MLOG("[waits] up to frame %llu, per frame:%s", (unsigned long long)frame, o ? line : " none");
}
#include "nvn_rec.h"   // deferred encoding (DM_NVN_ENC_THREAD)
static NSHashTable *g_res_objects;
static _Atomic unsigned g_image_heap_n;
static inline MTL4VisibilityOptions resource_vis(void) { return MTL4VisibilityOptionDevice | (g_image_heap_n ? MTL4VisibilityOptionResourceAlias : 0); }
// Residency churn by call site (the power line every 300 frames, mtl_res_stats): each commit with changes is a kernel call whose cost grows with what was added or removed
enum { RS_SITES = 16 }; static struct { int line; uint64_t n, bytes; } g_rs_site[RS_SITES]; static uint64_t g_rs_rm, g_rs_commits, g_rs_commit_ns;
#define res_add(r) res_add_at((r), __LINE__)
// Both run on guest threads too (pool_res from mtl_addr, uploads), whose implicit autorelease pools never drain; containsObject: / removeObject: on a weak NSHashTable leave the object
// autoreleased in the calling thread's pool (measured in a standalone test: the object outlived its last strong reference until the thread ended), so each has a pool of its own
static void res_add_at(id<MTLAllocation> allocation, int line) { @autoreleasepool {
    if (!allocation) return;
    // An indirectly addressed placed texture needs its heap resident. The heap owner below removes it only after the last texture/view has gone.
    id<MTLAllocation> r = [(id)allocation conformsToProtocol:@protocol(MTLResource)] && ((id<MTLResource>)allocation).heap ? (id)((id<MTLResource>)allocation).heap : allocation;
    pthread_mutex_lock(&g_res_mu);
    if (!g_res_objects) g_res_objects = [NSHashTable weakObjectsHashTable];
    if ([g_res_objects containsObject:r]) { pthread_mutex_unlock(&g_res_mu); return; }   // already resident (a shared texture resolved again): marking the set dirty would cost a commit that changes nothing
    { int k = 0; while (k < RS_SITES - 1 && g_rs_site[k].line && g_rs_site[k].line != line) k++; g_rs_site[k].line = line; g_rs_site[k].n++; g_rs_site[k].bytes += r.allocatedSize; }
    [g_res addAllocation:(id<MTLAllocation>)r];
    [g_res_objects addObject:r];
    g_res_dirty = 1;
    pthread_mutex_unlock(&g_res_mu);
} }
static void res_remove(id<MTLAllocation> r) { @autoreleasepool {
    if (!r) return;
    if ([(id)r conformsToProtocol:@protocol(MTLResource)] && ((id<MTLResource>)r).heap) return;
    pthread_mutex_lock(&g_res_mu);
    if (![g_res_objects containsObject:r]) { pthread_mutex_unlock(&g_res_mu); return; }
    [g_res removeAllocation:(id<MTLAllocation>)r];
    [g_res_objects removeObject:r];
    g_res_dirty = 1; g_rs_rm++;
    pthread_mutex_unlock(&g_res_mu);
} }
@interface NvImageHeap : NSObject { @public id<MTLHeap> heap; } @end
@implementation NvImageHeap
- (void)dealloc { res_remove(heap); atomic_fetch_sub(&g_image_heap_n, 1); }
@end
static char g_image_heap_owner_key;
// DM_RES_CHECK=1 (diagnostic): if textures / buffers the shader accesses through the argument block are not in the residency set, print the first 40 (textures are attributed to the actual allocation via parent texture / underlying buffer)
static int g_res_check_on;
static void res_check(id r, const char *what, int reg) {
    static int nbad;
    if (!g_res_check_on || !r || nbad >= 40) return;
    int is_tex = [r conformsToProtocol:@protocol(MTLTexture)], is_buf = [r conformsToProtocol:@protocol(MTLBuffer)];
    if (!is_tex && !is_buf) return;   // Samplers etc. are not allocations
    id<MTLResource> a = r; int hops = 0;
    if (is_tex) { id<MTLTexture> t = (id<MTLTexture>)a; while (t.parentTexture) { t = t.parentTexture; hops++; } a = t.buffer ? (id<MTLResource>)t.buffer : (id<MTLResource>)t; }
    pthread_mutex_lock(&g_res_mu); int ok = [g_res_objects containsObject:a.heap ? (id)a.heap : a]; pthread_mutex_unlock(&g_res_mu);
    if (!ok) { nbad++; id<MTLTexture> t = is_tex ? (id<MTLTexture>)r : nil;
        MLOG("[RED] missing residency #%d: %s slot %d %s %p (parent chain %d deep) allocated %lu B storage mode %lu%s", nbad, what, reg, is_tex ? "texture" : "buffer", (__bridge void *)a, hops, (unsigned long)a.allocatedSize, (unsigned long)a.storageMode,
             t ? [NSString stringWithFormat:@" format %lu %lux%lu type %lu usage %#lx", (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.textureType, (unsigned long)t.usage].UTF8String : ""); }
}
static void res_commit(void) {
    pthread_mutex_lock(&g_res_mu);
    if (g_res_dirty) { uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); [g_res commit]; g_res_dirty = 0; g_rs_commits++; g_rs_commit_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0; }
    pthread_mutex_unlock(&g_res_mu);
}
// Since the last call: residency commits (count, time), additions by source line (count, MB; the four largest by bytes) and removals
void mtl_res_stats(char *out, size_t n) {
    pthread_mutex_lock(&g_res_mu);
    uint64_t tn = 0, tb = 0; for (int k = 0; k < RS_SITES; k++) { tn += g_rs_site[k].n; tb += g_rs_site[k].bytes; }
    size_t o = snprintf(out, n, "commits %llu / %.1f ms, added %llu, %.0f MB (", (unsigned long long)g_rs_commits, g_rs_commit_ns / 1e6, (unsigned long long)tn, tb / 1048576.0);
    for (int i = 0; i < 4 && o < n; i++) { int best = -1; for (int k = 0; k < RS_SITES; k++) if (g_rs_site[k].n && (best < 0 || g_rs_site[k].bytes > g_rs_site[best].bytes)) best = k;
        if (best < 0) break; o += snprintf(out + o, n - o, "%sline %d: %llu, %.0f MB", i ? ", " : "", g_rs_site[best].line, (unsigned long long)g_rs_site[best].n, g_rs_site[best].bytes / 1048576.0); g_rs_site[best].n = 0; }
    if (o < n) snprintf(out + o, n - o, "), removed %llu", (unsigned long long)g_rs_rm);
    memset(g_rs_site, 0, sizeof g_rs_site); g_rs_rm = g_rs_commits = g_rs_commit_ns = 0;
    pthread_mutex_unlock(&g_res_mu);
}

@interface NvDeferredRelease : NSObject { @public id object; uint64_t fence; int keep_resident; } @end
@implementation NvDeferredRelease @end
static NSMutableArray<NvDeferredRelease *> *g_deferred;
static pthread_mutex_t g_deferred_mu = PTHREAD_MUTEX_INITIALIZER;
// Deferred release has two meanings that must be kept apart (the root of the GPU memory fault on the first frame on entering Travincal):
//   defer_obj_release = this object is retired: drop the reference after the submissions that used it complete, and remove it from the residency set;
//   defer_obj_unref   = only the reference I hold is no longer wanted, the object itself may still be in use (the view cache of a texture ID may hold the base texture itself): only drop the reference, leave the residency set alone.
// Previously only the former existed: when the view cache was invalidated, a texture that 'is still the current storage of some serial' was also removed from the residency set. Invalidation used to happen only when the base texture was really replaced, so it was never hit;
// after the transparency chain's in-place composite swapped the storage of two textures, each of the two live textures was removed once this way, and afterwards they are used as attachments and sampled every frame as usual - the GPU accesses non-resident memory.
static void defer_obj_release_ex(id obj, int keep_resident) {
    if (!obj) return;
    NvDeferredRelease *d = [NvDeferredRelease new]; d->object = obj; d->fence = g_fence + 1; d->keep_resident = keep_resident;
    pthread_mutex_lock(&g_deferred_mu);
    if (!g_deferred) g_deferred = [NSMutableArray array];
    [g_deferred addObject:d];
    pthread_mutex_unlock(&g_deferred_mu);
}
static void defer_obj_release(id obj) { defer_obj_release_ex(obj, 0); }
static void defer_obj_unref(id obj) { defer_obj_release_ex(obj, 1); }
// Its own autorelease pool: mtl_sub_begin drains before it pushes the submission's pool, on the guest submit thread, whose implicit pool is never drained. The weak-table lookups and the residency-set removal
// autorelease the retired object, so whatever that thread drained (instead of the completion handler, which wins the race only when the CPU is idle) stayed alive for good: in Metroid Dread every
// re-upload of its map mask kept the old texture and its 32 MB upload staging block, and GPU memory grew by several GB within minutes under CPU load.
static void defer_drain(void) { @autoreleasepool {
    NSMutableArray<NvDeferredRelease *> *ready = [NSMutableArray array];
    pthread_mutex_lock(&g_deferred_mu);
    for (NSInteger i = g_deferred.count - 1; i >= 0; i--) {
        NvDeferredRelease *d = g_deferred[(NSUInteger)i];
        if (d->fence <= g_done) { [ready addObject:d]; [g_deferred removeObjectAtIndex:(NSUInteger)i]; }
    }
    pthread_mutex_unlock(&g_deferred_mu);
    for (NvDeferredRelease *d in ready) {
        id obj = d->object;
        if (!d->keep_resident && [obj conformsToProtocol:@protocol(MTLResource)]) {
            pthread_mutex_lock(&g_res_mu);
            int registered = [g_res_objects containsObject:obj];
            pthread_mutex_unlock(&g_res_mu);
            if (registered) res_remove((id<MTLResource>)obj);
        }
        d->object = nil;
    }
} }

// A profile list of shader keys the host asks the next shader build for (bf_want.tsv, dual_want.tsv): each key appended once to the file the environment names. A shader's key is
// its 16 hex digits, with or without the route suffix (NvShader keys are "<key>_b"), as dmsh_build.py reads the list
typedef struct { const char *env; pthread_mutex_t mu; char **keys; size_t n, cap; int loaded; } want_list;
static void want_note(want_list *w, const char *key) {
    const char *path = getenv(w->env);
    if (!path || !key || strlen(key) < 16 || (key[16] && key[16] != '_')) return;
    for (int i = 0; i < 16; i++) if (!((key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'f') || (key[i] >= 'A' && key[i] <= 'F'))) return;
    char k16[17]; memcpy(k16, key, 16); k16[16] = 0;
    pthread_mutex_lock(&w->mu);
    if (!w->loaded) {
        FILE *f = fopen(path, "r"); char line[64];
        if (f) { while (fgets(line, sizeof line, f)) { char *e = strpbrk(line, "\r\n"); if (e) *e = 0; if (strlen(line) != 16) continue;
                if (w->n == w->cap) { w->cap = w->cap ? w->cap * 2 : 64; w->keys = realloc(w->keys, w->cap * sizeof *w->keys); }
                w->keys[w->n++] = strdup(line); } fclose(f); }
        w->loaded = 1;
    }
    for (size_t i = 0; i < w->n; i++) if (!strcmp(w->keys[i], k16)) { pthread_mutex_unlock(&w->mu); return; }
    if (w->n == w->cap) { w->cap = w->cap ? w->cap * 2 : 64; w->keys = realloc(w->keys, w->cap * sizeof *w->keys); }
    w->keys[w->n++] = strdup(k16);
    FILE *f = fopen(path, "a"); if (f) { fprintf(f, "%s\n", k16); fclose(f); }
    pthread_mutex_unlock(&w->mu);
}
static want_list g_bf_want = {"DM_NVN_BF_WANT", PTHREAD_MUTEX_INITIALIZER}, g_dual_want = {"DM_NVN_DUAL_WANT", PTHREAD_MUTEX_INITIALIZER};
void bf_want_note(const char *key) { want_note(&g_bf_want, key); }

// ---------- Memory pools ----------
typedef struct { uintptr_t lo, hi; uintptr_t base; __unsafe_unretained id<MTLBuffer> buf; uint64_t gpu; const void *key; int res; } pool_ent;   // res: already in the residency set (on demand, see pool_res)
static pool_ent *g_pools; static int g_npool, g_cap;
static int g_pidx_ok, g_plast = -1;   // whether the address→pool index is valid, the last hit (pool_find)
static NSMutableArray *g_keep;                  // Holds the buffer references (pool_ent has non-owning pointers)
static uint64_t g_pool_adds;
static int g_pool_res_n;   // registered pools already in the residency set (for the memory readout; under g_mu)

#define C1_BANK_SIZE (4u << 20)
#define C1_DECL_SIZE (64u << 10)
static NSMutableArray *g_c1_banks;
static size_t g_c1_bank_off, g_c1_alloc_bytes, g_c1_data_bytes;
static uint64_t g_c1_count, g_c1_exclusive;
static NSMutableSet<NSString *> *g_c1_full;
static int g_c1_full_valid;
static NSDictionary<NSString *, NSString *> *g_cb_ext;   // cb_extent.tsv: shader key → "reg:bytes ..." (how far into each uniform slot the shader can read); nil when stale or missing
static os_unfair_lock g_c1_lock = OS_UNFAIR_LOCK_INIT;

// DM_UDC_SELFTEST=1 (diagnostic, checks DontCare behavior on a real A19 device): private-storage Depth32Float_Stencil8 / RGBA8 (1952x900) are first cleared to write known values,
// then Load + DontCare without drawing, read back and compared pixel by pixel, printing one result line
static void udc_selftest(void) {
    if (!getenv("DM_UDC_SELFTEST")) return;
    id<MTLCommandQueue> q = [g_dev newCommandQueue];
    for (int depth = 1; depth >= 0; depth--) {
        NSUInteger w = 1952, h = 900;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depth ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        for (int pass = 0; pass < 2; pass++) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            MTLLoadAction la = pass ? MTLLoadActionLoad : MTLLoadActionClear; MTLStoreAction sa = pass ? MTLStoreActionDontCare : MTLStoreActionStore;
            if (depth) { rp.depthAttachment.texture = t; rp.depthAttachment.loadAction = la; rp.depthAttachment.storeAction = sa; rp.depthAttachment.clearDepth = 0.3125;
                         rp.stencilAttachment.texture = t; rp.stencilAttachment.loadAction = la; rp.stencilAttachment.storeAction = sa; rp.stencilAttachment.clearStencil = 0x5a; }
            else { rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = la; rp.colorAttachments[0].storeAction = sa; rp.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.4, 0.6, 0.8); }
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        }
        id<MTLBuffer> out = [g_dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:out destinationOffset:0
            destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4 options:depth ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        const uint32_t *px = out.contents; NSUInteger bad = 0; for (NSUInteger i = 0; i < w * h; i++) if (px[i] != px[0]) bad++;
        MLOG("UDC self-check %s %s %lux%lu first value %08x, values differing from it %lu / %lu", g_dev.name.UTF8String, depth ? "Depth32F_S8" : "RGBA8", (unsigned long)w, (unsigned long)h, px[0], (unsigned long)bad, (unsigned long)(w * h));
    }
}
void mtl_init(void) {
    if (g_dev) return;
    const char *vbc = getenv("DM_NVN_VBUF_CACHE"); g_vbuf_cache = !(vbc && !strcmp(vbc, "0"));
    const char *link = getenv("DM_PRESENT_LINK");
    g_dm_present_link = link ? atoi(link) : 1; if (g_dm_present_link < 0 || g_dm_present_link > 2) g_dm_present_link = 1;   // Default 1 (real-device same-process alternation: when load exceeds one display-link period, long frames 15% → 0 and frame intervals even out; no difference from 0 at a steady 60); DM_PRESENT_LINK=0 turns it off   // 1 = present on every tick of a 60 Hz display link; 2 = 120 Hz display link, the game presents a frame every other display-link tick (when behind, it can present on the next 8.33 ms display-link tick)
    const char *latency = getenv("DM_PRESENT_LATENCY");
    if (latency && atoi(latency) > 0) g_present_latency = atoi(latency);
    g_res_check_on = getenv("DM_RES_CHECK") != NULL;
    g_dev = MTLCreateSystemDefaultDevice();
    if (!g_dev) MDIE("no Metal device");
    if (!g_dev.hasUnifiedMemory) MDIE("needs a unified-memory device (zero-copy memory pools)");
    g_q = [g_dev newCommandQueue];
    // A device without Metal 4 (the paravirtualized GPU of a virtual Mac, such as GitHub's macos-26 runner: no residency set, no MTL4CommandQueue) cannot run the NVN layer. The GL and
    // Vulkan layers draw on classic queues of their own and need only the device (mtl_device_classic): there the classic part starts and presents are dropped, which is all a
    // run-time shader capture needs (the NSP script's capture_shaders boots headless; GitHub issue #112: Balatro's capture aborted here)
    NSError *re = nil; const char *no4 = NULL;
    if (getenv("DM_TEST_NO_METAL4")) no4 = "DM_TEST_NO_METAL4";   // test switch: as on a virtual Mac; not set in the product
    else if (![g_dev supportsFamily:MTLGPUFamilyMetal4]) no4 = "no Metal 4 GPU family";
    else {
        MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new];
        rd.initialCapacity = 4096;
        g_res = [g_dev newResidencySetWithDescriptor:rd error:&re];
        if (!g_res) no4 = "failed to create the MTLResidencySet";
    }
    if (!no4) {
        dispatch_queue_attr_t feedback_attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
        g_feedback_q = dispatch_queue_create("dm.nvn.mtl4-feedback", feedback_attr);
        MTL4CommandQueueDescriptor *q4d = [MTL4CommandQueueDescriptor new]; q4d.feedbackQueue = g_feedback_q;
        g_q4 = [g_dev newMTL4CommandQueueWithDescriptor:q4d error:&re];
        if (!g_q4) no4 = "failed to create the MTL4CommandQueue";
    }
    if (no4) {
        const char *why = re ? re.localizedDescription.UTF8String : "";
        if (!g_classic_ok) MDIE("the Metal device %s has no Metal 4 (%s%s%s), which the NVN layer needs; a virtual Mac's paravirtualized GPU has none", g_dev.name.UTF8String, no4, *why ? ": " : "", why);
        g_res = nil; g_q4 = nil; g_classic = 1;
        g_ev = [g_dev newSharedEvent];
        g_keep = [NSMutableArray array];
        MLOG("[RED] Metal device %s has no Metal 4 (%s%s%s): the classic part only, nothing is presented (enough for a run-time shader capture)", g_dev.name.UTF8String, no4, *why ? ": " : "", why);
        return;
    }
    // BC textures are created and uploaded as BC, with no decompression path (dm/gfx/tex/README.md); without BC the first one aborts in Metal's descriptor validation mid-game
    // (GitHub issue #704: an iPhone 15, A16). The iOS front end refuses such a device before the launcher (main.m); this stops every other route at the device, with the reason
    if (!g_dev.supportsBCTextureCompression) MDIE("the Metal device %s has no BC texture formats, which the games' textures use and the host does not decompress: an iPhone needs an A17 Pro or later, an iPad an M1 or later", g_dev.name.UTF8String);
    [g_q addResidencySet:g_res];
    [g_q4 addResidencySet:g_res];
    { void crash_set_wait_dump(void (*fn)(void)); void mtl_wait_state_dump(void); crash_set_wait_dump(mtl_wait_state_dump); }
    g_alloc_n = 3;
    for (int i = 0; i < 3; i++) {
        MTL4CommandAllocatorDescriptor *ad = [MTL4CommandAllocatorDescriptor new];
        ad.label = [NSString stringWithFormat:@"nvn-mtl allocator %d", i];
        NSError *ae = nil;
        g_alloc4[i] = [g_dev newCommandAllocatorWithDescriptor:ad error:&ae];
        if (!g_alloc4[i]) MDIE("failed to create MTL4CommandAllocator[%d]: %s", i, ae.localizedDescription.UTF8String);
        g_up_alloc4[i] = [g_dev newCommandAllocatorWithDescriptor:ad error:&ae];
        if (!g_up_alloc4[i]) MDIE("failed to create upload MTL4CommandAllocator[%d]: %s", i, ae.localizedDescription.UTF8String);
        MTL4CommandAllocatorDescriptor *pad = [MTL4CommandAllocatorDescriptor new];
        pad.label = [NSString stringWithFormat:@"nvn-mtl present allocator %d", i];
        g_present_alloc4[i] = [g_dev newCommandAllocatorWithDescriptor:pad error:&ae];
        if (!g_present_alloc4[i]) MDIE("failed to create present MTL4CommandAllocator[%d]: %s", i, ae.localizedDescription.UTF8String);
        g_present_cb[i] = [g_dev newCommandBuffer];
        if (!g_present_cb[i]) MDIE("failed to create present MTL4CommandBuffer[%d]", i);
    }
    MTL4ArgumentTableDescriptor *atd = [MTL4ArgumentTableDescriptor new];
    atd.maxBufferBindCount = 31; atd.maxTextureBindCount = 1;
    NSError *ate = nil;
    g_vt = rec_table([g_dev newArgumentTableWithDescriptor:atd error:&ate]); if (!g_vt) MDIE("failed to create the vertex argument table: %s", ate.localizedDescription.UTF8String);
    g_vt_valid = 0;
    g_ft = rec_table([g_dev newArgumentTableWithDescriptor:atd error:&ate]); if (!g_ft) MDIE("failed to create the fragment argument table: %s", ate.localizedDescription.UTF8String);
    g_ct = rec_table([g_dev newArgumentTableWithDescriptor:atd error:&ate]); if (!g_ct) MDIE("failed to create the compute argument table: %s", ate.localizedDescription.UTF8String);
    g_up_ct = [g_dev newArgumentTableWithDescriptor:atd error:&ate]; if (!g_up_ct) MDIE("failed to create the upload compute argument table: %s", ate.localizedDescription.UTF8String);
    MTL4ArgumentTableDescriptor *pad = [MTL4ArgumentTableDescriptor new];
    pad.maxTextureBindCount = 1; pad.maxSamplerStateBindCount = 1; pad.maxBufferBindCount = 1;
    g_present_args = [g_dev newArgumentTableWithDescriptor:pad error:&ate];
    if (!g_present_args) MDIE("failed to create the present argument table: %s", ate.localizedDescription.UTF8String);
    for (int i = 0; i < 3; i++) {
        g_present_uv[i] = [g_dev newBufferWithLength:sizeof(float) * 8 options:MTLResourceStorageModeShared];
        if (!g_present_uv[i]) MDIE("failed to create present UV buffer[%d]", i);
        res_add(g_present_uv[i]);
    }
    g_ev = [g_dev newSharedEvent];
    g_keep = [NSMutableArray array];
    g_nvn_deswz_gpu = nvn_deswz_gpu_init(g_dev) == 0;
    dm_res_report_register(mtl_res_counts);
    if (!g_nvn_deswz_gpu) MLOG("[RED] GPU deswizzle init failed; swizzled textures fall back to the CPU");
    MLOG("Metal device %s", g_dev.name.UTF8String);
    udc_selftest();
}

void dm_mtl_l3_tick(uint32_t tick) {
    static int leak = -1;
    if (leak < 0) leak = getenv("DM_TEST_LEAK_GPU") != NULL;
    if (leak && tick % 100 == 0) {
        if (!g_l3_gpu_leaks) g_l3_gpu_leaks = [NSMutableArray array];
        id<MTLBuffer> leak = [g_dev newBufferWithLength:1 << 20 options:MTLResourceStorageModeShared];
        if (leak) [g_l3_gpu_leaks addObject:leak];
    }
}

uint64_t dm_mtl_l3_sample(uint32_t tick) {
    (void)tick;
    return g_dev ? g_dev.currentAllocatedSize : 0;
}

// No-copy buffers of finalized pools (weak references, under g_mu): the memory report counts how many are still alive and their length -- a buffer that outlives its pool is still strongly referenced by something else (a texture built directly on the pool, etc.)
static NSHashTable *g_dead_pools;
// Address → pool cache per thread (mtl_addr / mtl_gpu_addr), valid while g_pool_gen is unchanged (every pool add / removal bumps it under g_mu), holding only pools already in the residency set.
// Every buffer binding of every draw used to take g_mu, which game threads creating pools and the GPU completion handler also take, and search the pool index. Two levels: a table direct-mapped
// by 4 KB page, then the last 8 pools found, which also fill the table. Breath of the Wild binds from more pools a frame than 8 entries alone could hold (about 2300 of its 21000 lookups a frame
// took the lock), and its uniform rings move to a new page every few draws (the table alone missed as often)
enum { PC_BITS = 10, PC_RECENT = 8 };
static _Atomic uint64_t g_pool_gen = 1;
typedef struct { uintptr_t lo, hi, base; __unsafe_unretained id<MTLBuffer> buf; uint64_t gpu; } pc_ent;
static __thread struct { uint64_t gen; unsigned next; pc_ent r[PC_RECENT], e[1u << PC_BITS]; } t_pc;   // an empty entry has hi 0
static inline unsigned pc_slot(uint64_t addr) { return (unsigned)(((addr >> 12) * 0x9E3779B97F4A7C15ull) >> (64 - PC_BITS)); }
static inline const pc_ent *pc_get(uint64_t addr, uint64_t len) {
    if (t_pc.gen != atomic_load_explicit(&g_pool_gen, memory_order_acquire)) return NULL;
    pc_ent *c = &t_pc.e[pc_slot(addr)];
    if (addr >= c->lo && addr + len <= c->hi) return c;
    for (unsigned k = 0; k < PC_RECENT; k++) { const pc_ent *x = &t_pc.r[k]; if (addr >= x->lo && addr + len <= x->hi) { *c = *x; return c; } }
    return NULL;
}
static void pc_put(int i, uint64_t addr) {   // caller holds g_mu, pool i is resident and holds addr
    uint64_t gen = atomic_load_explicit(&g_pool_gen, memory_order_relaxed);
    if (t_pc.gen != gen) { memset(t_pc.r, 0, sizeof t_pc.r); memset(t_pc.e, 0, sizeof t_pc.e); t_pc.gen = gen; }
    pc_ent c = {g_pools[i].lo, g_pools[i].hi, g_pools[i].base, g_pools[i].buf, g_pools[i].gpu};
    t_pc.e[pc_slot(addr)] = c; t_pc.r[t_pc.next++ % PC_RECENT] = c;
}
static _Atomic uint64_t g_reg_gen = 1;   // Change generation of memory pool / texture / sampler registration: one of the invalidation conditions for argument buffer reuse
// On-demand residency of memory pools: putting a zero-copy buffer into the residency set makes iOS read the whole pool into memory and pin it as wired, even a pool the game never uses
// (they measured on a 17 Pro Max a 512 MB render-target pool addressed 0 times in a run, with about 530 MB more device wired memory; a 4 GB A14 was ended by the system within seconds of launch because of it).
// So a pool is not put into the residency set when created; it is put in, and the residency set committed on the spot, the first time the GPU gets its address through mtl_addr / mtl_gpu_addr -- these are the only two ways the GPU gets pool addresses,
// and committing on the spot is earlier than any command buffer commit that uses it. CPU-side reads of pool contents through mtl_addr trigger it too (conservative, never misses). Caller holds g_mu
static void pool_res(int i) {
    if (g_pools[i].res) return;
    res_add(g_pools[i].buf); res_commit(); g_pools[i].res = 1; g_pool_res_n++;
}
void mtl_pool_add(const void *mem, uint64_t size) {
    mtl_init(); g_reg_gen++;
    uintptr_t pg = (uintptr_t)getpagesize(), lo = (uintptr_t)mem, hi = lo + size;
    uintptr_t a = lo & ~(pg - 1), b = (hi + pg - 1) & ~(pg - 1);
    id<MTLBuffer> buf = [g_dev newBufferWithBytesNoCopy:(void *)a length:b - a options:MTLResourceStorageModeShared deallocator:nil];
    // Pokémon Legends: Z-A: twice on the Mac a small pool's buffer was refused although its pages were one read-write VM region, both times while another heavy job (a shader compile,
    // a second game instance) took the machine's memory; every run on an idle machine created all of them. Wiring the pages may fail under memory pressure: retry for up to about 2 s
    for (int t = 0; !buf && t < 12; t++) {
        usleep(10000u << (t < 4 ? t : 4));
        buf = [g_dev newBufferWithBytesNoCopy:(void *)a length:b - a options:MTLResourceStorageModeShared deallocator:nil];
        if (buf) MLOG("[RED] memory pool %p+%#llx: the zero-copy buffer was refused, created at retry %d", mem, (unsigned long long)size, t + 1);
    }
    if (!buf) {   // say what the pages looked like: every VM region over the range, with its protection (an unmapped page in the range is the usual cause)
        for (vm_address_t r = a; r < b; ) {
            vm_size_t rs = 0; vm_region_basic_info_data_64_t vi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
            if (vm_region_64(mach_task_self(), &r, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&vi, &cnt, &obj) != KERN_SUCCESS) { MLOG("[RED]   no VM region at or after %#llx", (unsigned long long)r); break; }
            MLOG("[RED]   VM region %#llx+%#llx protection %d (max %d)", (unsigned long long)r, (unsigned long long)rs, vi.protection, vi.max_protection);
            r += rs;
        }
        MDIE("memory pool %p+%#llx: failed to create the zero-copy buffer over %#lx..%#lx", mem, (unsigned long long)size, (unsigned long)a, (unsigned long)b);
    }
    pthread_mutex_lock(&g_mu);
    if (g_npool == g_cap) { g_cap = g_cap ? g_cap * 2 : 64; g_pools = realloc(g_pools, g_cap * sizeof *g_pools); }
    g_pidx_ok = 0; g_plast = -1; atomic_fetch_add(&g_pool_gen, 1);   // the address index and the per-thread caches are invalid (pool_find, pc_get)
    g_pools[g_npool++] = (pool_ent){lo, hi, a, buf, buf.gpuAddress, mem, 0};   // not in the residency set yet: put in the first time it is addressed (pool_res)
    g_pool_adds++;
    [g_keep addObject:buf];
    pthread_mutex_unlock(&g_mu);
}

void mtl_pool_del(const void *mem) {
    g_reg_gen++;
    pthread_mutex_lock(&g_mu);
    // The no-copy buffer goes through deferred release (out of the residency set and unreferenced after submitted commands complete). It used to be removed only from g_keep while the residency set still held a strong reference: after the pool was freed the buffer stayed alive wrapping that guest memory,
    // and a new pool at the same address wrapped it again. Engage's title demo cycles scenes; GPU allocated grew to 5.4 GB and resident memory to 3.6 GB in 850 s.
    // Its own autorelease pool: this runs on the guest thread that finalizes the pool, whose implicit pool never drains, and adding to the weak table g_dead_pools left the buffer
    // autoreleased there, so every finalized pool buffer stayed alive for good (Ghost Trick: Phantom Detective, 125 buffers and 6.6 GB of GPU allocations 300 frames into the boot, 0 with the pool)
    @autoreleasepool {
    for (int i = 0; i < g_npool; i++)
        if (g_pools[i].key == mem) { id<MTLBuffer> b = g_pools[i].buf; [g_keep removeObject:b]; defer_obj_release(b); if (g_pools[i].res) g_pool_res_n--;
            if (!g_dead_pools) g_dead_pools = [NSHashTable weakObjectsHashTable];
            [g_dead_pools addObject:b]; g_pools[i] = g_pools[--g_npool]; g_pidx_ok = 0; g_plast = -1; atomic_fetch_add(&g_pool_gen, 1); break; }
    }
    pthread_mutex_unlock(&g_mu);
}

// Address → pool: the last hit first, then a binary search over indices sorted by start address, and a full linear scan only when that fails (pools overlapping).
// The Legend of Zelda: Breath of the Wild keeps about 1760 memory pools alive and every buffer binding of every draw looks one up; the old full scan took about 4% of the submit thread's samples (mtl_gpu_addr 222 / 6 s). Adding / removing pools invalidates the index, which is re-sorted when next used. Caller holds g_mu
static int *g_pidx; static int g_pidx_cap;
static int pidx_cmp(const void *a, const void *b) { uintptr_t x = g_pools[*(const int *)a].lo, y = g_pools[*(const int *)b].lo; return x < y ? -1 : x > y; }
static int pool_find(uint64_t addr, uint64_t len) {
    if (g_plast >= 0 && g_plast < g_npool && addr >= g_pools[g_plast].lo && addr + len <= g_pools[g_plast].hi) return g_plast;
    if (!g_pidx_ok) {
        if (g_pidx_cap < g_npool) { g_pidx_cap = g_npool * 2; g_pidx = realloc(g_pidx, (size_t)g_pidx_cap * sizeof *g_pidx); }
        for (int i = 0; i < g_npool; i++) g_pidx[i] = i;
        qsort(g_pidx, (size_t)g_npool, sizeof *g_pidx, pidx_cmp); g_pidx_ok = 1;
    }
    int lo = 0, hi = g_npool - 1, hit = -1;   // the last one whose start ≤ addr
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_pools[g_pidx[m]].lo <= addr) { hit = m; lo = m + 1; } else hi = m - 1; }
    if (hit >= 0 && addr + len <= g_pools[g_pidx[hit]].hi) return g_plast = g_pidx[hit];
    for (int i = 0; i < g_npool; i++) if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi) return g_plast = i;
    return -1;
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    const pc_ent *c = pc_get(addr, len);
    if (c) { *buf = (__bridge void *)c->buf; *off = addr - c->base; return 1; }
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    int i = pool_find(addr, len);
    if (i >= 0) { pool_res(i); pc_put(i, addr); *buf = (__bridge void *)g_pools[i].buf; *off = addr - g_pools[i].base; ok = 1; }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
int mtl_gpu_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off, uint64_t *gpu) {
    const pc_ent *c = pc_get(addr, len);
    if (c) { *buf = (__bridge void *)c->buf; *off = addr - c->base; *gpu = c->gpu + *off; return 1; }
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    int i = pool_find(addr, len);
    if (i >= 0) { pool_res(i); pc_put(i, addr); *buf = (__bridge void *)g_pools[i].buf; *off = addr - g_pools[i].base; *gpu = g_pools[i].gpu + *off; ok = 1; }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

#include "nvn_mtl_submit.inc"
#include "nvn_mtl_ubuf.inc"
#include "nvn_mtl_tex.inc"
#include "nvn_mtl_pass.inc"
#include "nvn_mtl_shader.inc"
#include "nvn_mtl_state.inc"
#include "nvn_mtl_pso.inc"
#include "nvn_mtl_args.inc"
#include "nvn_mtl_lookahead.inc"
#include "nvn_mtl_merge.inc"
#include "nvn_mtl_indirect.inc"
#include "nvn_mtl_draw.inc"
#include "nvn_mtl_shot.inc"
#include "nvn_mtl_present.inc"
#ifdef NVN_RT_RANGE_TEST
#include "rt_range_check.inc"
#endif

// Called by the watchdog (no progress): writes the state of all blocking wait points on the graphics side to the log -- who is waiting for whom is visible at a glance (after switching to the Metal 4 submit model there was once a case of "ran to 900 frames then stopped, no failure line")
void mtl_wait_state_dump(void) {
    MLOG("wait points: main fence submitted %llu / completed %llu; main allocator fences [%llu %llu %llu]; uploads pending %d, allocators busy [%d %d %d], seq %u, unflushed batch %d; present queue in %llu / done %llu; main command buffer encoding %d, submit thread set %d",
         (unsigned long long)g_fence, (unsigned long long)g_done, (unsigned long long)g_alloc_fence[0], (unsigned long long)g_alloc_fence[1], (unsigned long long)g_alloc_fence[2],
         atomic_load(&g_up_pending), atomic_load(&g_up_alloc_busy[0]), atomic_load(&g_up_alloc_busy[1]), atomic_load(&g_up_alloc_busy[2]), g_up_seq, g_up_cb != nil,
         (unsigned long long)g_pq_in, (unsigned long long)g_pq_done, g_cb != nil, atomic_load(&g_sub_thread_set));
}
void mtl_alloc_stats(uint64_t *waits, uint64_t *grown, int *n) { *waits = g_alloc_waits; *grown = g_alloc_grown; *n = g_alloc_n; }   // Main submit allocator pool: how many times it waited, how many times it grew, how many exist now
#include "nvn_capture.inc"   // DM_NVN_CAPTURE (one frame for dm/gfx/tests/nvn_replay)
#pragma clang attribute pop
