// iOS 17 traditional Metal backend, ported from D2R compatibility mechanisms.
// Metal execution layer of the NVN null device, see nvn_mtl.h.
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <fcntl.h>
#include <TargetConditionals.h>
#if TARGET_OS_OSX || __IPHONE_OS_VERSION_MIN_REQUIRED >= 170000
#define DM_HAVE_METAL_DISPLAY_LINK 1
#else
#define DM_HAVE_METAL_DISPLAY_LINK 0
#endif
#if TARGET_OS_IPHONE
#include <os/proc.h>   // os_proc_available_memory: for the memory readout line
#endif
#include "nvn_mtl.h"
#include "nvn_prepare.h"
#include "nvn_obj.h"
#include "../tex/nvn_fmt_mtl.h"
#include "../tex/nvn_deswizzle.h"
#include "../tex/nvn_deswizzle_gpu.h"
#include "../tex/nvn_bcdec.h"
#include "../tex/nvn_bc2astc.h"
#include "../tex/bc2astc_msl.h"   // nvn_bca_msl: BC1/2/3 -> ASTC 4x4 GPU transcoder
#include "nvn_builtin.h"
#include "nvn_dirty.h"
#include "nvn_refl.h"
#include "nvn_cover.h"
#include "dmsh_chain.h"   // DMSH_CHAIN: the translation chain version of the B shader library expected by this source tree (in the game profile folder, -I from the Makefile)
#include "game_profile.h"   // game profile constants (NVN driver constant segment bases, profile id)
// Texture slots 0-127 hold combined NVN handles. Slots 128-255 hold the
// separate texture bindings produced by nvnCommandBufferBindSeparateTexture.
#define NTEX 256
// Unified NVN driver constant layout used by the current shader translator.
// c2: combined textures, images, separate textures, then separate samplers.
// c0: storage-buffer descriptors at a stage-specific base.
enum { C2_TEX = 0x20, C2_TEX_SLOTS = 41, C2_IMG = 0x120, C2_IMG_SLOTS = 8,
       C2_SEPTEX = 0x168, C2_SAMP = 0x568, DRV_SIZE = 0xA00 };
_Static_assert(C2_TEX + 8 * C2_TEX_SLOTS == C2_SEPTEX &&
               C2_SEPTEX + 8 * 128 == C2_SAMP &&
               C2_SAMP + 8 * 128 <= DRV_SIZE &&
               NVN_SEPTEX_SLOT0 + 128 == NTEX,
               "NVN driver handle tables");
static int c0_ssbo_base(int stage) {
    return stage == 0 ? 0x110 : stage == 1 ? 0x510 : stage == 5 ? 0x310 : -1;
}
#include "nvn_hmap.h"
#include "../../host/dm.h"
#import <CommonCrypto/CommonDigest.h>
#include <stdarg.h>
#include <mach-o/dyld.h>
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
// Function parameters in this file are never retained again in the callee (the caller / global tables already hold them, and lifetime is guaranteed by defer_obj_release): saves a retain / release pair per call on the hot path
#pragma clang attribute push(__attribute__((objc_externally_retained)), apply_to = function)

#define MLOG(...) do { fprintf(stderr, "[dm] [nvn-mtl] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define MDIE(...) do { MLOG("FATAL " __VA_ARGS__); abort(); } while (0)

static void warm_start(void);
static void warm_claim(void);
static _Atomic int g_prepare_state, g_prepare_frontend;
static _Atomic uint32_t g_prepare_total, g_prepare_done, g_prepare_failed, g_prepare_skipped;
static pthread_mutex_t g_prepare_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_prepare_cv = PTHREAD_COND_INITIALIZER;
static int g_prepare_active = 1;
void mtl_prepare_active(int active) {
    pthread_mutex_lock(&g_prepare_mu); g_prepare_active = active;
    pthread_cond_broadcast(&g_prepare_cv); pthread_mutex_unlock(&g_prepare_mu);
}
static void prepare_foreground_wait(void) {
    if (!atomic_load(&g_prepare_frontend)) return;
    pthread_mutex_lock(&g_prepare_mu);
    while (!g_prepare_active) pthread_cond_wait(&g_prepare_cv, &g_prepare_mu);
    pthread_mutex_unlock(&g_prepare_mu);
}
void mtl_prepare_snapshot(nvn_prepare_status *out) {
    // State publication pairs with READY/PARTIAL after all PSO tables are claimed.
    out->state = atomic_load(&g_prepare_state);
    out->total = atomic_load(&g_prepare_total); out->completed = atomic_load(&g_prepare_done);
    out->failed = atomic_load(&g_prepare_failed); out->skipped = atomic_load(&g_prepare_skipped);
    nvn_cache_snapshot(&out->cache);
}

static id<MTLDevice> g_dev;
static int g_layered_native = 1, g_bare_native = 1, g_fragment_image_native = 1, g_slice_replaying;
static uint32_t g_selected_slice;
static NSMutableArray *g_l3_gpu_leaks;
static int g_nvn_deswz_gpu;
static id<MTLCommandQueue> g_q;


static _Atomic uint32_t g_present_qos;
#define ALLOC_MAX 8
static int g_alloc_n, g_alloc_cur;
static int g_vbuf_cache = 1, g_buffer_offset_cache = 1;
static uint64_t g_vbuf_resolve, g_vbuf_reuse, g_buffer_offset_calls;
static id<MTLBuffer> g_present_uv[3];
static NSString *g_shmap_path;
static void pso_pend_harvest(void);
int g_bloom_q = -1;   // Bloom 1/4 starting point (non-static: real-device same-process alternation sets it to 0 / 1 directly)

static id<MTLBuffer> g_bloomq_const[3];
static id<MTLSamplerState> g_bloomq_samp;
static id<MTLRenderPipelineState> g_bloomq_pso;
static MTLPixelFormat g_bloomq_fmt;
static id<MTLTexture> g_bloomq_view;
static int g_bloomq_i;
static struct { int state; uint64_t s_ser, b0_ser, b1_ser, s_h, samp_h, b1_h; const tex_t *s_tex; float threshold, coeff_g, coeff_b; } g_bloomq;
static uint64_t g_bloomq_last_b0, g_bloomq_last_b1h, g_bloomq_last_b1s;   // Most recent bloom 1/4 overlay read redirection: B0 serial → B1 handle (kept across frames, see the 2e2d6700a2569795 branch)
static uint64_t g_present_alloc_fence[3];
static pthread_mutex_t g_pq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pq_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_link_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_link_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_link_tick_ns, g_link_tick_n;
static uint64_t g_link_dropped;
static double g_link_hold_until; static uint64_t g_link_held;   // Changed only on the display-link callback thread: the time the previous frame must stay on screen until by its present interval (0 = no hold), and the ticks that took no new frame because of it
static int g_pq_inflight;   // Under g_pq_mu: number of presents committed to the GPU whose completion callback has not arrived yet
// Under g_pq_mu: claim the layer before attaching a display link, and keep each
// classic frame protected from dequeue until its scheduled present call returns.
static int g_link_owns_layer, g_classic_pending;
int g_dm_present_link;   // Present pacing mode (runtime value of DM_PRESENT_LINK, modified directly during same-process alternating measurement): 0 = acquire the drawable itself; 1 = display link at 60 Hz; 2 = display link at 120 Hz, the game presents a frame every other display-link tick
static int g_present_latency = 1;
static uint64_t g_pq_done;
static uint64_t g_pq_read;

// On-device render diagnostics. These stay behind environment switches and
// publish the complete Objective-C object set once before parallel pipeline
// builders read it.
static dispatch_once_t g_hilite_once;
static id<MTLFunction> g_hilite_fs[6], g_hilite_vs;
static char g_hilite_keys[6][24];
static int g_hilite_n, g_hilite_full;
static NSMutableDictionary<NSString *, id<MTLTexture>> *g_diag_c0_cache;

static int g_res_dirty;
static id<MTLSharedEvent> g_ev;
static _Atomic uint64_t g_fence;                // Highest committed fence value (incremented exclusively by the submit thread, read by guest registration threads)
static _Atomic uint64_t g_sub_serial;            // Submission index (also sampled by deferred-release diagnostics on guest registration threads)
static _Atomic uint64_t g_sub_pending_fence;     // Fence owned by the active replay, published before g_sub_active
static _Atomic int g_sub_active;                 // A main replay exists and may have encoded references not covered by g_done yet
static _Atomic uint64_t g_cut_committed_diag;    // Host segments committed so far; diagnostic only, never guest-visible
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_done;                         // Highest completed fence value (written by the completion callback)
static uint64_t g_arena_made;
static int g_allocprof = -1; static uint64_t g_ap_bytes[64], g_ap_fn[8];
#define AP_B uint64_t _ap0 = g_allocprof > 0 ? g_dev.currentAllocatedSize : 0
#define AP_E(i) do { if (g_allocprof > 0) { uint64_t _n = g_dev.currentAllocatedSize; if (_n > _ap0) g_ap_fn[i] += _n - _ap0; } } while (0)   // Per-frame arenas created so far (4 MiB each; for the memory report)
static _Atomic int g_last_commit_error;
static _Atomic int g_up_pending;

static pthread_mutex_t g_res_mu = PTHREAD_MUTEX_INITIALIZER;   // Dedicated lock for the residency set: res_add is called on paths that hold g_mu (arena_new), so it cannot share g_mu
static NSHashTable *g_res_objects;
#include "nvn_legacy_buffers.h"
#include "nvn_argument_encoding.h"
// Residency churn by call site (the power line every 300 frames, mtl_res_stats): each commit with changes is a kernel call whose cost grows with what was added or removed
enum { RS_SITES = 16 }; static struct { int line; uint64_t n, bytes; } g_rs_site[RS_SITES]; static uint64_t g_rs_rm, g_rs_commits, g_rs_commit_ns;
#define res_add(r) res_add_at((r), __LINE__)
// These paths also run on guest threads whose implicit autorelease pools do not
// drain.  NSHashTable membership calls autorelease their arguments, so bound
// that lifetime here instead of retaining finalized Metal resources forever.
static void res_add_at(id<MTLResource> r, int line) { @autoreleasepool {
    if (!r) return;
    pthread_mutex_lock(&g_res_mu);
    if ([r conformsToProtocol:@protocol(MTLBuffer)]) legacy_buffer_add((id<MTLBuffer>)r);
    if (!g_res_objects) g_res_objects = [NSHashTable weakObjectsHashTable];
    if ([g_res_objects containsObject:r]) { pthread_mutex_unlock(&g_res_mu); return; }   // already resident (a shared texture resolved again): marking the set dirty would cost a commit that changes nothing
    { int k = 0; while (k < RS_SITES - 1 && g_rs_site[k].line && g_rs_site[k].line != line) k++; g_rs_site[k].line = line; g_rs_site[k].n++; g_rs_site[k].bytes += r.allocatedSize; }
    [g_res_objects addObject:r];
    g_res_dirty = 1;
    pthread_mutex_unlock(&g_res_mu);
} }
static void res_remove(id<MTLResource> r) { @autoreleasepool {
    if (!r) return;
    pthread_mutex_lock(&g_res_mu);
    if ([r conformsToProtocol:@protocol(MTLBuffer)]) legacy_buffer_remove((id<MTLBuffer>)r);
    if (![g_res_objects containsObject:r]) { pthread_mutex_unlock(&g_res_mu); return; }
    [g_res_objects removeObject:r];
    g_res_dirty = 1; g_rs_rm++;
    pthread_mutex_unlock(&g_res_mu);
} }
// DM_RES_CHECK=1 (diagnostic): if textures / buffers the shader accesses through the argument block are not in the residency set, print the first 40 (textures are attributed to the actual allocation via parent texture / underlying buffer)
static int g_res_check_on;
static void res_check(id r, const char *what, int reg) {
    static int nbad;
    if (!g_res_check_on || !r || nbad >= 40) return;
    int is_tex = [r conformsToProtocol:@protocol(MTLTexture)], is_buf = [r conformsToProtocol:@protocol(MTLBuffer)];
    if (!is_tex && !is_buf) return;   // Samplers etc. are not allocations
    id<MTLResource> a = r; int hops = 0;
    if (is_tex) { id<MTLTexture> t = (id<MTLTexture>)a; while (t.parentTexture) { t = t.parentTexture; hops++; } a = t.buffer ? (id<MTLResource>)t.buffer : (id<MTLResource>)t; }
    pthread_mutex_lock(&g_res_mu); int ok = [g_res_objects containsObject:a]; pthread_mutex_unlock(&g_res_mu);
    if (!ok) { nbad++; id<MTLTexture> t = is_tex ? (id<MTLTexture>)r : nil;
        MLOG("【红】驻留缺失 #%d：%s 槽 %d %s %p（父链 %d 层）分配 %lu B 存储 %lu%s", nbad, what, reg, is_tex ? "贴图" : "缓冲", (__bridge void *)a, hops, (unsigned long)a.allocatedSize, (unsigned long)a.storageMode,
             t ? [NSString stringWithFormat:@" 格式 %lu %lux%lu 类型 %lu 用途 %#lx", (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.textureType, (unsigned long)t.usage].UTF8String : ""); }
}
static void res_commit(void) {
    pthread_mutex_lock(&g_res_mu);
    if (g_res_dirty) { g_res_dirty = 0; }
    pthread_mutex_unlock(&g_res_mu);
}
// Since the last call: residency commits (count, time), additions by source line (count, MB; the four largest by bytes) and removals
void mtl_res_stats(char *out, size_t n) {
    pthread_mutex_lock(&g_res_mu);
    uint64_t tn = 0, tb = 0; for (int k = 0; k < RS_SITES; k++) { tn += g_rs_site[k].n; tb += g_rs_site[k].bytes; }
    size_t o = snprintf(out, n, "提交 %llu 次 / %.1f ms，加 %llu 个 %.0f MB（", (unsigned long long)g_rs_commits, g_rs_commit_ns / 1e6, (unsigned long long)tn, tb / 1048576.0);
    for (int i = 0; i < 4 && o < n; i++) { int best = -1; for (int k = 0; k < RS_SITES; k++) if (g_rs_site[k].n && (best < 0 || g_rs_site[k].bytes > g_rs_site[best].bytes)) best = k;
        if (best < 0) break; o += snprintf(out + o, n - o, "%s行 %d：%llu 个 %.0f MB", i ? "，" : "", g_rs_site[best].line, (unsigned long long)g_rs_site[best].n, g_rs_site[best].bytes / 1048576.0); g_rs_site[best].n = 0; }
    if (o < n) snprintf(out + o, n - o, "），删 %llu 个", (unsigned long long)g_rs_rm);
    memset(g_rs_site, 0, sizeof g_rs_site); g_rs_rm = g_rs_commits = g_rs_commit_ns = 0;
    pthread_mutex_unlock(&g_res_mu);
}

@interface NvDeferredRelease : NSObject { @public
    id object;
    uint64_t fence, sub_serial, direct_bytes, root_ref_bytes;
    int site;
    uint8_t keep_resident, cut_eligible, submission_active, replay_view_retirement;
    uint8_t kind, is_view;   // kind: 0 other, 1 buffer, 2 texture, 3 other MTLResource
} @end
@implementation NvDeferredRelease @end
static NSMutableArray<NvDeferredRelease *> *g_deferred;
static pthread_mutex_t g_deferred_mu = PTHREAD_MUTEX_INITIALIZER;
// Replay-only derived-view records may become releasable while their caller still holds a
// registry lock. Enqueue only requests a drain; public registry entry points
// perform it after dropping their lock so object destruction cannot invert the
// registry/residency lock order.
static _Atomic int g_defer_replay_view_drain_needed;
// g_sub_active and g_sub_pending_fence are atomic for diagnostics, but every
// state transition and every retirement decision is serialized by
// g_deferred_mu. An acquire load alone may legally observe an older false while
// begin races a guest registration thread; the mutex gives that race one exact
// order instead.
static void defer_submission_begin_state(void) {
    pthread_mutex_lock(&g_deferred_mu);
    uint64_t pending = atomic_load_explicit(&g_fence, memory_order_relaxed) + 1;
    atomic_store_explicit(&g_sub_pending_fence, pending, memory_order_relaxed);
    atomic_store_explicit(&g_sub_active, 1, memory_order_relaxed);
    pthread_mutex_unlock(&g_deferred_mu);
}
static uint64_t defer_submission_prepare_commit(void) {
    pthread_mutex_lock(&g_deferred_mu);
    uint64_t fence = atomic_load_explicit(&g_fence, memory_order_relaxed) + 1;
    uint64_t pending = atomic_load_explicit(&g_sub_pending_fence, memory_order_relaxed);
    int good = atomic_load_explicit(&g_sub_active, memory_order_relaxed) && fence == pending;
    if (good) atomic_store_explicit(&g_fence, fence, memory_order_release);
    pthread_mutex_unlock(&g_deferred_mu);
    if (!good) MDIE("submission fence publication mismatch: committed %llu pending %llu active=%d",
                    (unsigned long long)fence, (unsigned long long)pending,
                    atomic_load_explicit(&g_sub_active, memory_order_relaxed));
    return fence;
}
static void defer_submission_did_commit(uint64_t fence) {
    pthread_mutex_lock(&g_deferred_mu);
    uint64_t published = atomic_load_explicit(&g_fence, memory_order_relaxed);
    uint64_t pending = atomic_load_explicit(&g_sub_pending_fence, memory_order_relaxed);
    int good = atomic_load_explicit(&g_sub_active, memory_order_relaxed) && published == fence && pending == fence;
    if (good) atomic_store_explicit(&g_sub_active, 0, memory_order_relaxed);
    pthread_mutex_unlock(&g_deferred_mu);
    if (!good) MDIE("submission close mismatch: committed %llu published %llu pending %llu",
                    (unsigned long long)fence, (unsigned long long)published, (unsigned long long)pending);
}
enum { DEFER_DIAG_SITES = 48 };
typedef struct {
    int site;
    uint64_t count, direct_bytes, root_ref_bytes, cut, keep, active, replay_view, views;
    uint64_t buffers, textures, resources, other, first_sub, last_sub;
} defer_diag_site;
typedef struct {
    uint64_t count, direct_bytes, root_ref_bytes, cut, keep, active, replay_view, views;
    uint64_t buffers, textures, resources, other, first_sub, last_sub, first_fence, last_fence;
    defer_diag_site sites[DEFER_DIAG_SITES];
    int nsites;
} defer_diag_summary;
static _Atomic unsigned g_defer_diag_thresholds;
static int defer_diag_on(void) {
    static dispatch_once_t once; static int on;
    dispatch_once(&once, ^{ on = getenv("DM_NVN_DEFER_DIAG") != NULL; });
    return on;
}
static void defer_diag_add(defer_diag_summary *s, NvDeferredRelease *d) {
    s->count++; s->direct_bytes += d->direct_bytes; s->root_ref_bytes += d->root_ref_bytes;
    if (s->count == 1) {
        s->first_sub = s->last_sub = d->sub_serial;
        s->first_fence = s->last_fence = d->fence;
    } else {
        if (d->sub_serial < s->first_sub) s->first_sub = d->sub_serial;
        if (d->sub_serial > s->last_sub) s->last_sub = d->sub_serial;
        if (d->fence < s->first_fence) s->first_fence = d->fence;
        if (d->fence > s->last_fence) s->last_fence = d->fence;
    }
    s->cut += d->cut_eligible; s->keep += d->keep_resident; s->active += d->submission_active;
    s->replay_view += d->replay_view_retirement; s->views += d->is_view;
    s->buffers += d->kind == 1; s->textures += d->kind == 2; s->resources += d->kind == 3; s->other += d->kind == 0;
    int k = 0; while (k < s->nsites && s->sites[k].site != d->site) k++;
    if (k == s->nsites) {
        if (k >= DEFER_DIAG_SITES) return;
        s->nsites++; s->sites[k].site = d->site; s->sites[k].first_sub = d->sub_serial;
    }
    defer_diag_site *p = &s->sites[k];
    p->count++; p->direct_bytes += d->direct_bytes; p->root_ref_bytes += d->root_ref_bytes;
    p->cut += d->cut_eligible; p->keep += d->keep_resident; p->active += d->submission_active;
    p->replay_view += d->replay_view_retirement; p->views += d->is_view;
    p->buffers += d->kind == 1; p->textures += d->kind == 2; p->resources += d->kind == 3; p->other += d->kind == 0;
    if (d->sub_serial < p->first_sub) p->first_sub = d->sub_serial;
    if (d->sub_serial > p->last_sub) p->last_sub = d->sub_serial;
}
static void defer_diag_collect_locked(defer_diag_summary *s) {
    memset(s, 0, sizeof *s);
    for (NvDeferredRelease *d in g_deferred) defer_diag_add(s, d);
}
static void defer_diag_snapshot(const char *why) { @autoreleasepool {
    if (!defer_diag_on()) return;
    defer_diag_summary s; pthread_mutex_lock(&g_deferred_mu); defer_diag_collect_locked(&s); pthread_mutex_unlock(&g_deferred_mu);
    uint64_t current_sub = atomic_load_explicit(&g_sub_serial, memory_order_relaxed);
    uint64_t pending_age = s.count && current_sub >= s.first_sub ? current_sub - s.first_sub : 0;
    MLOG("deferred-release diagnostic (%s): queued=%llu direct=%lluMB root-reference-sum=%lluMB replay-eligible=%llu keep-resident=%llu active-submit=%llu replay-view-retirements=%llu views=%llu types=%lluB/%lluT/%lluR/%lluO submissions=%llu..%llu age=%llu fences=%llu..%llu done=%llu cuts=%llu",
         why, (unsigned long long)s.count, (unsigned long long)(s.direct_bytes >> 20), (unsigned long long)(s.root_ref_bytes >> 20),
         (unsigned long long)s.cut, (unsigned long long)s.keep, (unsigned long long)s.active, (unsigned long long)s.replay_view,
         (unsigned long long)s.views, (unsigned long long)s.buffers, (unsigned long long)s.textures,
         (unsigned long long)s.resources, (unsigned long long)s.other,
         (unsigned long long)s.first_sub, (unsigned long long)s.last_sub, (unsigned long long)pending_age,
         (unsigned long long)s.first_fence, (unsigned long long)s.last_fence,
         (unsigned long long)atomic_load_explicit(&g_done, memory_order_relaxed),
         (unsigned long long)atomic_load_explicit(&g_cut_committed_diag, memory_order_relaxed));
    uint8_t used[DEFER_DIAG_SITES] = {0};
    for (int rank = 0; rank < 6; rank++) {
        int best = -1; for (int i = 0; i < s.nsites; i++) if (!used[i] && (best < 0 || s.sites[i].count > s.sites[best].count)) best = i;
        if (best < 0 || !s.sites[best].count) break; used[best] = 1; defer_diag_site *p = &s.sites[best];
        uint64_t site_age = current_sub >= p->first_sub ? current_sub - p->first_sub : 0;
        MLOG("deferred-release diagnostic top%d: line=%d count=%llu direct=%lluMB root-reference-sum=%lluMB replay-eligible=%llu keep=%llu active=%llu replay-view=%llu views=%llu types=%lluB/%lluT/%lluR/%lluO submissions=%llu..%llu age=%llu",
             rank + 1, p->site, (unsigned long long)p->count, (unsigned long long)(p->direct_bytes >> 20),
             (unsigned long long)(p->root_ref_bytes >> 20), (unsigned long long)p->cut, (unsigned long long)p->keep,
             (unsigned long long)p->active, (unsigned long long)p->replay_view, (unsigned long long)p->views,
             (unsigned long long)p->buffers, (unsigned long long)p->textures, (unsigned long long)p->resources,
             (unsigned long long)p->other, (unsigned long long)p->first_sub, (unsigned long long)p->last_sub,
             (unsigned long long)site_age);
    }
} }
// Set only on the thread while it is replaying one NVN submission. A retired
// object queued there cannot be looked up by a later replay segment: the call
// that queues it has already removed / replaced the owning serial, view, pool
// or scratch slot. Retirements from other guest threads stay on the original
// whole-submission fence because they may race command recording / replay.
static _Thread_local int g_defer_cut_active;
// Deferred release has two meanings that must be kept apart (the root of the GPU memory fault on the first frame on entering Travincal):
//   defer_obj_release = this object is retired: drop the reference after the submissions that used it complete, and remove it from the residency set;
//   defer_obj_unref   = only the reference I hold is no longer wanted, the object itself may still be in use (the view cache of a texture ID may hold the base texture itself): only drop the reference, leave the residency set alone.
// Previously only the former existed: when the view cache was invalidated, a texture that 'is still the current storage of some serial' was also removed from the residency set. Invalidation used to happen only when the base texture was really replaced, so it was never hit;
// after the transparency chain's in-place composite swapped the storage of two textures, each of the two live textures was removed once this way, and afterwards they are used as attachments and sampled every frame as usual - the GPU accesses non-resident memory.
static void defer_drain(void);
static void defer_obj_release_ex(id obj, int keep_resident, int replay_view_retirement, int site) {
    if (!obj) return;
    NvDeferredRelease *d = [NvDeferredRelease new]; d->object = obj; d->keep_resident = keep_resident;
    d->cut_eligible = g_defer_cut_active; d->replay_view_retirement = replay_view_retirement;
    d->site = site; d->sub_serial = atomic_load_explicit(&g_sub_serial, memory_order_relaxed);
    if ([obj conformsToProtocol:@protocol(MTLBuffer)]) {
        id<MTLBuffer> b = obj; d->kind = 1; d->direct_bytes = d->root_ref_bytes = MAX((uint64_t)b.allocatedSize, (uint64_t)b.length);
    } else if ([obj conformsToProtocol:@protocol(MTLTexture)]) {
        id<MTLTexture> t = obj, root = t; d->kind = 2; d->is_view = t.parentTexture != nil || t.buffer != nil;
        while (root.parentTexture) root = root.parentTexture;
        id<MTLResource> storage = root.buffer ? (id<MTLResource>)root.buffer : (id<MTLResource>)root;
        d->root_ref_bytes = storage.allocatedSize;
        if (!d->is_view) d->direct_bytes = d->root_ref_bytes;
    } else if ([obj conformsToProtocol:@protocol(MTLResource)]) {
        d->kind = 3; d->direct_bytes = d->root_ref_bytes = ((id<MTLResource>)obj).allocatedSize;
    }
    NSUInteger queued; uint64_t fence; int active;
    pthread_mutex_lock(&g_deferred_mu);
    active = atomic_load_explicit(&g_sub_active, memory_order_relaxed);
    fence = active ? atomic_load_explicit(&g_sub_pending_fence, memory_order_relaxed) :
            atomic_load_explicit(&g_fence, memory_order_relaxed) + !replay_view_retirement;
    d->fence = fence; d->submission_active = active;
    if (!g_deferred) g_deferred = [NSMutableArray array];
    [g_deferred addObject:d];
    queued = g_deferred.count;
    if (replay_view_retirement && fence <= atomic_load_explicit(&g_done, memory_order_acquire))
        atomic_store_explicit(&g_defer_replay_view_drain_needed, 1, memory_order_release);
    pthread_mutex_unlock(&g_deferred_mu);
    // Threshold diagnostics run only after releasing g_deferred_mu: snapshot
    // takes that same lock while aggregating the queue.
    if (defer_diag_on()) {
        unsigned bit = queued >= 1024 ? 4u : queued >= 512 ? 2u : queued >= 128 ? 1u : 0u;
        if (bit && !(atomic_fetch_or(&g_defer_diag_thresholds, bit) & bit)) defer_diag_snapshot("queue threshold");
    }
    // Never drain here: descriptor registration callers hold g_rmu. Releasing
    // a Metal object can enter residency bookkeeping, so defer that work to the
    // explicit post-unlock safe point below.
}
#define defer_obj_release(obj) defer_obj_release_ex((obj), 0, 0, __LINE__)
#define defer_obj_unref(obj) defer_obj_release_ex((obj), 1, 0, __LINE__)
#define defer_replay_view_unref(obj) defer_obj_release_ex((obj), 1, 1, __LINE__)
static void defer_replay_view_drain_safe_point(void) {
    if (atomic_exchange_explicit(&g_defer_replay_view_drain_needed, 0, memory_order_acq_rel)) defer_drain();
}
static void defer_release_batch(NSArray<NvDeferredRelease *> *ready) { @autoreleasepool {
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
// Drain retired objects on the guest submit thread as well as completion threads.
// Upstream 0fc0ce43b184: the guest thread has no draining implicit autorelease pool.
static void defer_drain(void) { @autoreleasepool {
    NSMutableArray<NvDeferredRelease *> *ready = [NSMutableArray array];
    pthread_mutex_lock(&g_deferred_mu);
    for (NSInteger i = g_deferred.count - 1; i >= 0; i--) {
        NvDeferredRelease *d = g_deferred[(NSUInteger)i];
        if (d->fence <= g_done) { [ready addObject:d]; [g_deferred removeObjectAtIndex:(NSUInteger)i]; }
    }
    pthread_mutex_unlock(&g_deferred_mu);
    defer_release_batch(ready);
} }

// Take only objects retired by this replay thread before the current split.
// The returned array is captured by that Metal command buffer's completion
// handler. Guest-visible fence completion remains on the terminal segment.
static NSArray<NvDeferredRelease *> *defer_take_cut_batch(void) { @autoreleasepool {
    NSMutableArray<NvDeferredRelease *> *ready = nil;
    pthread_mutex_lock(&g_deferred_mu);
    uint64_t fence = atomic_load_explicit(&g_sub_pending_fence, memory_order_relaxed);
    for (NSInteger i = g_deferred.count - 1; i >= 0; i--) {
        NvDeferredRelease *d = g_deferred[(NSUInteger)i];
        if (d->cut_eligible && d->fence == fence) {
            if (!ready) ready = [NSMutableArray array];
            [ready addObject:d];
            [g_deferred removeObjectAtIndex:(NSUInteger)i];
        }
    }
    pthread_mutex_unlock(&g_deferred_mu);
    return ready.count ? [ready copy] : nil;
} }

static pthread_mutex_t g_bf_want_mu = PTHREAD_MUTEX_INITIALIZER;
static char **g_bf_want_keys; static size_t g_bf_want_n, g_bf_want_cap; static int g_bf_want_loaded;
void bf_want_note(const char *key) {
    const char *path = getenv("DM_NVN_BF_WANT");
    if (!path || !key || strlen(key) != 16) return;
    for (int i = 0; i < 16; i++) if (!((key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'f') || (key[i] >= 'A' && key[i] <= 'F'))) return;
    pthread_mutex_lock(&g_bf_want_mu);
    if (!g_bf_want_loaded) {
        FILE *f = fopen(path, "r"); char line[64];
        if (f) { while (fgets(line, sizeof line, f)) { char *e = strpbrk(line, "\r\n"); if (e) *e = 0; if (strlen(line) != 16) continue;
                if (g_bf_want_n == g_bf_want_cap) { g_bf_want_cap = g_bf_want_cap ? g_bf_want_cap * 2 : 64; g_bf_want_keys = realloc(g_bf_want_keys, g_bf_want_cap * sizeof *g_bf_want_keys); }
                g_bf_want_keys[g_bf_want_n++] = strdup(line); } fclose(f); }
        g_bf_want_loaded = 1;
    }
    for (size_t i = 0; i < g_bf_want_n; i++) if (!strcmp(g_bf_want_keys[i], key)) { pthread_mutex_unlock(&g_bf_want_mu); return; }
    if (g_bf_want_n == g_bf_want_cap) { g_bf_want_cap = g_bf_want_cap ? g_bf_want_cap * 2 : 64; g_bf_want_keys = realloc(g_bf_want_keys, g_bf_want_cap * sizeof *g_bf_want_keys); }
    g_bf_want_keys[g_bf_want_n++] = strdup(key);
    FILE *f = fopen(path, "a"); if (f) { fprintf(f, "%s\n", key); fclose(f); }
    pthread_mutex_unlock(&g_bf_want_mu);
}

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
        MLOG("UDC自检 %s %s %lux%lu 首值 %08x 与首值不同 %lu / %lu", g_dev.name.UTF8String, depth ? "Depth32F_S8" : "RGBA8", (unsigned long)w, (unsigned long)h, px[0], (unsigned long)bad, (unsigned long)(w * h));
    }
}
void mtl_init(void) {
    if (g_dev) return;
    const char *vbc = getenv("DM_NVN_VBUF_CACHE"); g_vbuf_cache = !vbc || strcmp(vbc, "0");
    const char *boc = getenv("DM_NVN_BUFFER_OFFSET"); g_buffer_offset_cache = !boc || strcmp(boc, "0");
    const char *link = getenv("DM_PRESENT_LINK");
    g_dm_present_link = link ? atoi(link) : 1; if (g_dm_present_link < 0 || g_dm_present_link > 2) g_dm_present_link = 1;   // Default 1 (real-device same-process alternation: when load exceeds one display-link period, long frames 15% → 0 and frame intervals even out; no difference from 0 at a steady 60); DM_PRESENT_LINK=0 turns it off   // 1 = present on every tick of a 60 Hz display link; 2 = 120 Hz display link, the game presents a frame every other display-link tick (when behind, it can present on the next 8.33 ms display-link tick)
    const char *latency = getenv("DM_PRESENT_LATENCY");
    if (latency && atoi(latency) > 0) g_present_latency = atoi(latency);
    g_res_check_on = getenv("DM_RES_CHECK") != NULL;
    g_dev = MTLCreateSystemDefaultDevice();
    if (!g_dev) MDIE("没有 Metal 设备");
    legacy_arguments_configure(g_dev);
    g_layered_native = [g_dev supportsFamily:MTLGPUFamilyApple5] || [g_dev supportsFamily:MTLGPUFamilyMac1];
    MLOG("layered rendering: %s", g_layered_native ? "native" : "verified per-slice companions required");
    g_bare_native = [g_dev supportsFamily:MTLGPUFamilyApple4] || [g_dev supportsFamily:MTLGPUFamilyMac2];
    g_fragment_image_native = [g_dev supportsFamily:MTLGPUFamilyApple4] || [g_dev supportsFamily:MTLGPUFamilyMac1];
    MLOG("attachment-less rendering: %s", g_bare_native ? "native" : "transient attachment preserving raster bounds");
    MLOG("argument buffers: tier=%s encoding=%s", g_dev.argumentBuffersSupport == MTLArgumentBuffersTier1 ? "1" : "2",
         g_legacy_direct_arguments ? "direct GPU addresses" : "MTLArgumentEncoder private layout");
    if (!g_dev.hasUnifiedMemory) MDIE("需要统一内存设备（零拷贝内存池）");
    g_q = [g_dev newCommandQueue];
    if (!g_q) MDIE("【红】Cannot create the iOS 17 command queue");
    g_alloc_n = 3;
    { void crash_set_wait_dump(void (*fn)(void)); void mtl_wait_state_dump(void); crash_set_wait_dump(mtl_wait_state_dump); }
    for (int i = 0; i < 3; i++) {
        g_present_uv[i] = [g_dev newBufferWithLength:sizeof(float) * 8 options:MTLResourceStorageModeShared];
        if (!g_present_uv[i]) MDIE("建立出图 UV 缓冲[%d] 失败", i);
        res_add(g_present_uv[i]);
        g_bloomq_const[i] = [g_dev newBufferWithLength:sizeof(float) * 8 options:MTLResourceStorageModeShared];
        if (!g_bloomq_const[i]) MDIE("建立泛光 1/4 常量缓冲[%d] 失败", i);
        res_add(g_bloomq_const[i]);
    }
    g_ev = [g_dev newSharedEvent];
    g_keep = [NSMutableArray array];
    g_nvn_deswz_gpu = nvn_deswz_gpu_init(g_dev) == 0;
    dm_res_report_register(mtl_res_counts);
    if (!g_nvn_deswz_gpu) MLOG("【红】显卡解排布初始化失败，预排布贴图回退 CPU");
    MLOG("Metal 设备 %s", g_dev.name.UTF8String);
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
// Address → pool cache per thread (mtl_addr / mtl_gpu_addr): 8 entries replaced in turn, valid while g_pool_gen is unchanged (every pool add / removal bumps it under g_mu), holding only pools
// already in the residency set. Every buffer binding of every draw used to take g_mu, which game threads creating pools and the GPU completion handler also take, and search the pool index
static _Atomic uint64_t g_pool_gen = 1;
typedef struct { uintptr_t lo, hi, base; __unsafe_unretained id<MTLBuffer> buf; uint64_t gpu; } pc_ent;
static __thread struct { uint64_t gen; unsigned n, next; pc_ent e[8]; } t_pc;
static inline const pc_ent *pc_get(uint64_t addr, uint64_t len) {
    if (t_pc.gen != atomic_load_explicit(&g_pool_gen, memory_order_acquire)) return NULL;
    for (unsigned k = 0; k < t_pc.n; k++) { const pc_ent *c = &t_pc.e[k]; if (addr >= c->lo && addr + len <= c->hi) return c; }
    return NULL;
}
static void pc_put(int i) {   // caller holds g_mu, pool i is resident
    uint64_t gen = atomic_load_explicit(&g_pool_gen, memory_order_relaxed);
    if (t_pc.gen != gen) { t_pc.gen = gen; t_pc.n = 0; t_pc.next = 0; }
    pc_ent *c = &t_pc.e[t_pc.n < 8 ? t_pc.n++ : t_pc.next++ % 8];
    *c = (pc_ent){g_pools[i].lo, g_pools[i].hi, g_pools[i].base, g_pools[i].buf, g_pools[i].gpu};
}
static _Atomic uint64_t g_reg_gen = 1;   // Change generation of memory pool / texture / sampler registration: one of the invalidation conditions for argument buffer reuse
// On-demand residency of memory pools (from d2r-mobile e2a0c9a9b): putting a zero-copy buffer into the residency set makes iOS read the whole pool into memory and pin it as wired, even a pool the game never uses
// (they measured on a 17 Pro Max a 512 MB render-target pool addressed 0 times in a run, with about 530 MB more device wired memory; a 4 GB A14 was ended by the system within seconds of launch because of it).
// So a pool is not put into the residency set when created; it is put in, and the residency set committed on the spot, the first time the GPU gets its address through mtl_addr / mtl_gpu_addr -- these are the only two ways the GPU gets pool addresses,
// and committing on the spot is earlier than any command buffer commit that uses it. CPU-side reads of pool contents through mtl_addr trigger it too (conservative, never misses). Caller holds g_mu
static void pool_res(int i) {
    if (g_pools[i].res) return;
    res_add(g_pools[i].buf); res_commit(); g_pools[i].res = 1; g_pool_res_n++;
}
// A failed no-copy registration is a CPU VM/buffer creation error. Report the
// actual host-page span rather than only the guest's smaller logical range.
static void pool_nocopy_failure(uintptr_t a, uintptr_t b, uintptr_t pg) {
    MLOG("no-copy pool failure: host-page=%#llx span=%#llx+%#llx max-buffer=%#llx allocated=%#llx",
         (unsigned long long)pg, (unsigned long long)a, (unsigned long long)(b - a),
         (unsigned long long)g_dev.maxBufferLength, (unsigned long long)g_dev.currentAllocatedSize);
    vm_address_t cursor = a;
    for (int n = 0; cursor < b && n < 16; n++) {
        vm_address_t start = cursor; vm_size_t length = 0;
        vm_region_submap_info_data_64_t info; natural_t depth = 0;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        kern_return_t kr;
        for (;;) {
            kr = vm_region_recurse_64(mach_task_self(), &start, &length, &depth,
                                       (vm_region_recurse_info_t)&info, &count);
            if (kr != KERN_SUCCESS || !info.is_submap) break;
            depth++; count = VM_REGION_SUBMAP_INFO_COUNT_64;
        }
        if (kr != KERN_SUCCESS || !length) { MLOG("no-copy pool VM query failed at %#llx: %d", (unsigned long long)cursor, kr); break; }
        MLOG("no-copy pool VM: %#llx+%#llx prot=%x max=%x share=%u external=%u gap=%d",
             (unsigned long long)start, (unsigned long long)length, info.protection,
             info.max_protection, info.share_mode, info.external_pager, start > cursor);
        if (start + length <= cursor) break;
        cursor = start + length;
    }
}
void mtl_pool_add(const void *mem, uint64_t size) {
    mtl_init(); g_reg_gen++;
    uintptr_t pg = (uintptr_t)getpagesize(), lo = (uintptr_t)mem, hi = lo + size;
    uintptr_t a = lo & ~(pg - 1), b = (hi + pg - 1) & ~(pg - 1);
    id<MTLBuffer> buf = [g_dev newBufferWithBytesNoCopy:(void *)a length:b - a options:MTLResourceStorageModeShared deallocator:nil];
    // Restored from the original backend: pinning an otherwise valid VM span
    // may be refused transiently while the guest maps/unmaps large regions.
    // Keep the original zero-copy storage and logical bounds; wait a bounded
    // interval before treating a temporary allocation refusal as fatal.
    for (int retry = 0; !buf && retry < 12; retry++) {
        usleep(10000u << (retry < 4 ? retry : 4));
        buf = [g_dev newBufferWithBytesNoCopy:(void *)a length:b - a options:MTLResourceStorageModeShared deallocator:nil];
        if (buf) MLOG("[RED] memory pool %p+%#llx: no-copy buffer created at retry %d", mem, (unsigned long long)size, retry + 1);
    }
    if (!buf) { pool_nocopy_failure(a, b, pg); MDIE("Cannot register no-copy memory pool %p+%#llx", mem, (unsigned long long)size); }
    pthread_mutex_lock(&g_mu);
    if (g_npool == g_cap) { g_cap = g_cap ? g_cap * 2 : 64; g_pools = realloc(g_pools, g_cap * sizeof *g_pools); }
    g_pidx_ok = 0; g_plast = -1; atomic_fetch_add(&g_pool_gen, 1);   // the address index and the per-thread caches are invalid (pool_find, pc_get)
    g_pools[g_npool++] = (pool_ent){lo, hi, a, buf, legacy_buffer_gpu(buf), mem, 0};   // not in the residency set yet: put in the first time it is addressed (pool_res)
    g_pool_adds++;
    [g_keep addObject:buf];
    pthread_mutex_unlock(&g_mu);
}

void mtl_pool_del(const void *mem) {
    g_reg_gen++;
    pthread_mutex_lock(&g_mu);
    // The no-copy buffer goes through deferred release (out of the residency set and unreferenced after submitted commands complete). It used to be removed only from g_keep while the residency set still held a strong reference: after the pool was freed the buffer stayed alive wrapping that guest memory,
    // and a new pool at the same address wrapped it again. Engage's title demo cycles scenes; GPU allocated grew to 5.4 GB and resident memory to 3.6 GB in 850 s.
    // This may run on a guest thread whose implicit autorelease pool never drains;
    // keep weak-table operations from extending the finalized buffer's lifetime.
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
    if (i >= 0) { pool_res(i); pc_put(i); *buf = (__bridge void *)g_pools[i].buf; *off = addr - g_pools[i].base; ok = 1; }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
int mtl_gpu_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off, uint64_t *gpu) {
    const pc_ent *c = pc_get(addr, len);
    if (c) { *buf = (__bridge void *)c->buf; *off = addr - c->base; *gpu = c->gpu + *off; return 1; }
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    int i = pool_find(addr, len);
    if (i >= 0) { pool_res(i); pc_put(i); *buf = (__bridge void *)g_pools[i].buf; *off = addr - g_pools[i].base; *gpu = g_pools[i].gpu + *off; ok = 1; }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

// ---------- Submission ----------
static void defer_reset(void); static void defer_fallback(void);
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; void *vis; } post_op;   // vis: the visibility slot table for kind 3
// ---------- Conditional rendering (profile render_cond) ----------
// Metal has no predication, so replay evaluates NVN's render-enable condition
// from its two counter records. Timestamp reports have a known counter value of
// zero; samples-passed reports remain unknown until completion and are drawn.
static int g_rc_skip;
#define RC_PEND 256
static struct { uint64_t addr; int kind; } g_rc_pend[RC_PEND];
static int g_rc_npend;
static uint64_t g_rc_occ[4096];
static void rc_occ_add(uint64_t a) {
    for (uint32_t i = (uint32_t)(a >> 4) & 4095, k = 0; k < 4096; k++, i = (i + 1) & 4095) {
        if (g_rc_occ[i] == a) return;
        if (!g_rc_occ[i]) { g_rc_occ[i] = a; return; }
    }
}
static int rc_occ_has(uint64_t a) {
    for (uint32_t i = (uint32_t)(a >> 4) & 4095, k = 0; k < 4096; k++, i = (i + 1) & 4095) {
        if (g_rc_occ[i] == a) return 1;
        if (!g_rc_occ[i]) return 0;
    }
    return 0;
}
static void rc_note(uint64_t a, int kind) {
    if (kind == 2) rc_occ_add(a);
    for (int i = 0; i < g_rc_npend; i++) if (g_rc_pend[i].addr == a) { g_rc_pend[i].kind = kind; return; }
    if (g_rc_npend < RC_PEND) g_rc_pend[g_rc_npend++] = (typeof(g_rc_pend[0])){a, kind};
}
static int rc_value(uint64_t a, uint64_t *v) {
    if (rc_occ_has(a)) return 0;
    for (int i = g_rc_npend - 1; i >= 0; i--) if (g_rc_pend[i].addr == a) {
        if (g_rc_pend[i].kind != 1) return 0;
        *v = 0;
        return 1;
    }
    *v = *(const volatile uint64_t *)a;
    return 1;
}
// Diagnostic A/B for games whose conditional-render profile has not yet been
// proven on the device. Keep decoding and logging the condition, but draw the
// command anyway. This is runtime-controlled so the same binary can be
// compared with and without the override.
static _Atomic int g_rc_always_draw = -1, g_force_visible = -1, g_min_reduction = -1;
static int gfx_diagnostic_bool(_Atomic int *state, const char *key) {
    int value = atomic_load_explicit(state, memory_order_acquire);
    if (value >= 0) return value;
    const char *e = getenv(key); value = e && !strcmp(e, "1");
    int expected = -1;
    // A live notification wins over an initializer that read the old env.
    if (!atomic_compare_exchange_strong_explicit(state, &expected, value, memory_order_acq_rel, memory_order_acquire)) return expected;
    if (value) MLOG("%s=1: graphics diagnostic initialized", key);
    return value;
}
// DM_CTL publishes the parsed value directly. Never reset to getenv here:
// completion and replay threads may read these diagnostic switches together.
void dm_gfx_env_changed(const char *key, const char *value) {
    if (!key) return;
    _Atomic int *state;
    if (!strcmp(key, "DM_NVN_RENDER_COND_ALWAYS_DRAW")) state = &g_rc_always_draw;
    else if (!strcmp(key, "DM_NVN_FORCE_VISIBLE")) state = &g_force_visible;
    else if (!strcmp(key, "DM_NVN_MIN_REDUCTION")) state = &g_min_reduction;
    else return;
    int on = value && !strcmp(value, "1");
    atomic_store_explicit(state, on, memory_order_release);
    MLOG("Live graphics diagnostic %s=%d", key, on);
}
static int rc_always_draw(void) {
    return gfx_diagnostic_bool(&g_rc_always_draw, "DM_NVN_RENDER_COND_ALWAYS_DRAW");
}
static void rc_override_note(const p_render_cond *rc, int would_skip, int known, uint64_t x, uint64_t y) {
    if (!would_skip || !rc_always_draw()) return;
    static uint64_t n;
    n++;
    if (n <= 64 || !(n % 600))
        MLOG("conditional-render override #%llu: mode=%d addr=%#llx known=%d values=%llu/%llu (draw kept)",
             (unsigned long long)n, rc->mode, (unsigned long long)rc->addr, known,
             (unsigned long long)x, (unsigned long long)y);
}
// ---------- Occlusion queries (NVN_COUNTER_TYPE_SAMPLES_PASSED) ----------
// NVN semantics: samples passed is a cumulative counter on the queue - ResetCounter zeroes it, every draw afterwards adds to it, and ReportCounter writes out the cumulative value at that moment (+ timestamp) when the GPU completes.
// Metal: every render pass attaches a visibility result buffer; draws during counting use counting mode, with a new slot per pass (no reliance on accumulation across passes); a reset empties the 'slot table since the reset',
// and a report hands the slot table together with its buffer (retained) to the completion callback to sum and write back; after a report a new slot is used, so later draws are not counted into the reported value.
// (Previously always wrote 0: occlusion query = 'fully occluded', campfires / light sources got culled - the character select screen calls ResetCounter 21 times per frame)
typedef struct { void *buf; uint32_t slot; } vis_slot;
typedef struct { int n; vis_slot s[]; } vis_list;
#define VIS_SLOTS 8192
typedef struct { __strong id<MTLBuffer> buf; uint64_t fence; } vis_buf;
static vis_buf g_vis_ring[4]; static int g_vis_ring_n, g_vis_ring_i = -1;
static id<MTLBuffer> g_vis; static uint32_t g_vis_n;   // This submission's visibility buffer and number of slots used
static int vis_in_ring(id<MTLBuffer> b) { for (int i = 0; i < g_vis_ring_n; i++) if (g_vis_ring[i].buf == b) return 1; return 0; }
static int g_vis_on, g_vis_counting, g_vis_cur = -1; static uint64_t g_vis_cur_pass;
static vis_slot *g_vs; static int g_nvs, g_vscap;      // Slots since the last reset
static uint64_t g_cnt_report[32], g_cnt_reset[32];       // Report / reset counts per counter type (diagnostics)
// iOS 16 exposes no sampler min/max reduction mode. Z-A uses a min-reduced
// depth pyramid for occlusion, so an averaged pyramid can report visible props
// as zero samples. This opt-in fallback promotes only zero results to visible.
static int vis_force_visible(void) {
    return gfx_diagnostic_bool(&g_force_visible, "DM_NVN_FORCE_VISIBLE");
}
static void vis_clear(void) { for (int i = 0; i < g_nvs; i++) CFRelease(g_vs[i].buf); g_nvs = 0; g_vis_cur = -1; }
static vis_list *vis_snapshot(void) {
    vis_list *l = malloc(sizeof *l + g_nvs * sizeof(vis_slot)); l->n = g_nvs;
    for (int i = 0; i < g_nvs; i++) { l->s[i] = g_vs[i]; CFRetain(g_vs[i].buf); }
    return l;
}
// Target ranges of buffer writes (copies / inline uniform updates) already encoded in this submission but not yet executed on the GPU: reading these addresses on the CPU side at encode time yields old values (uv_proof avoids them when reading uniforms)
#define PEND_MAX 256
static uint64_t g_pend_lo[PEND_MAX], g_pend_hi[PEND_MAX]; static int g_npend, g_pend_over;
static int pend_hits(uint64_t a, uint64_t n) { if (g_pend_over) return 1; for (int i = 0; i < g_npend; i++) if (a < g_pend_hi[i] && a + n > g_pend_lo[i]) return 1; return 0; }
// Persistent GPU-write ranges.  g_npend is reset at submission boundaries, but
// the CPU may replay the next submission while the previous one is still in
// flight.  Indirect arguments must therefore remember GPU writers across that
// boundary instead of trusting the current bytes in shared memory.
#define GPU_WRITE_MAX 512
typedef struct { uint64_t lo, hi, epoch; } gpu_write_range;
static gpu_write_range g_gpu_write[GPU_WRITE_MAX]; static int g_gpu_write_n;
static uint64_t g_gpu_write_epoch, g_gpu_write_synced_epoch, g_indirect_sync_sub;
static uint64_t g_gpu_write_over_epoch;
static void gpu_write_note(uint64_t a, uint64_t n) {
    if (!a || !n || a > UINT64_MAX - n) { g_gpu_write_over_epoch = ++g_gpu_write_epoch; return; }
    uint64_t hi = a + n, epoch = ++g_gpu_write_epoch;
    for (int i = 0; i < g_gpu_write_n; i++) if (a < g_gpu_write[i].hi && g_gpu_write[i].lo < hi) {
        if (a < g_gpu_write[i].lo) g_gpu_write[i].lo = a;
        if (hi > g_gpu_write[i].hi) g_gpu_write[i].hi = hi;
        g_gpu_write[i].epoch = epoch; return;
    }
    if (g_gpu_write_n < GPU_WRITE_MAX) g_gpu_write[g_gpu_write_n++] = (gpu_write_range){a, hi, epoch};
    else g_gpu_write_over_epoch = epoch;
}
static uint64_t gpu_write_hit_epoch(uint64_t a, uint64_t n) {
    uint64_t epoch = g_gpu_write_over_epoch;
    if (!a || !n || a > UINT64_MAX - n) return g_gpu_write_epoch;
    for (int i = 0; i < g_gpu_write_n; i++) if (a < g_gpu_write[i].hi && g_gpu_write[i].lo < a + n && g_gpu_write[i].epoch > epoch) epoch = g_gpu_write[i].epoch;
    return epoch;
}
static void pend_add(uint64_t a, uint64_t n) {
    if (!a || !n) return;
    if (g_npend < PEND_MAX && a <= UINT64_MAX - n) { g_pend_lo[g_npend] = a; g_pend_hi[g_npend++] = a + n; }
    else g_pend_over = 1;
    gpu_write_note(a, n);
}
typedef struct { uint64_t ser; uint16_t level, slice; } zskip_zero;
static zskip_zero g_zskip_zero[4096]; static int g_zskip_n, g_zskip_count;
static zskip_zero g_zskip_cand[8]; static uint8_t g_zskip_cand_ok[8];
static void zskip_clear(void);
static int tbdr_on(void);
static id g_cb;
static id<MTLBlitCommandEncoder> g_blit;
static NSMutableArray<dispatch_block_t> *g_commit_blocks;
static NSMutableArray<dispatch_block_t> *g_sub_argument_checks;
// Raw sampler resource IDs in argument buffers do not retain their objects.
// Keep each referenced sampler alive until this submission's GPU completion.
static NSMutableSet<id<MTLSamplerState>> *g_sub_samplers;
static NSMutableString *g_sub_sample_notes;
static uint64_t g_alloc_fence[ALLOC_MAX]; static uint64_t g_alloc_waits, g_alloc_grown;
int mtl_wait(uint64_t fence, uint64_t timeout_ns);
static void up_flush(void);
// The upload batch belongs only to the submit thread (the thread that called mtl_sub_begin): the batch's encoder cannot be used across threads. Every entry point asserts current thread == submit thread,
// and on mismatch prints the stack and aborts on the spot - so that 'another thread touched the batch' is exposed deterministically on Mac too, not by timing (real iOS device stuck at the main menu: the present thread flushed the submit thread's batch via q_cb).
static pthread_t g_sub_thread; static _Atomic int g_sub_thread_set;
static int on_sub_thread(void) { return atomic_load(&g_sub_thread_set) && pthread_equal(pthread_self(), g_sub_thread); }
static void up_owner_check(const char *where) {
    if (!atomic_load(&g_sub_thread_set) || on_sub_thread()) return;
    char name[64] = ""; pthread_getname_np(pthread_self(), name, sizeof name);
    MLOG("【红】上传批只属于提交线程：%s 在线程「%s」上被调用", where, name);
    void *bt[32]; int n = backtrace(bt, 32); backtrace_symbols_fd(bt, n, 2);
    abort();
}
// Every command buffer created outside the replay / upload batch goes through this. On the submit thread: flush this batch's uploads first, so that commands that then read or write these textures are ordered after the uploads;
// other threads (the present thread etc.) creating a command buffer do not touch the batch: they do not read or write textures uploaded in this batch; commands that need these textures all come after this replay submission (the batch is flushed before it)
// Old-queue command buffers (readback / diagnostics / present; to be deleted once M4-4 and M4-5 are migrated away): uploads are already on the Metal 4 queue, and there is no ordering guarantee between the two queues -
// wait for the upload to complete after flushing it, and only then can the old queue read the upload result (mtl_read_texels waited first and flushed afterwards, the readback beat the upload, 4 failures on t_nvn upload content)
static void up_wait_zero(_Atomic int *v);
static id<MTLCommandBuffer> q_cb(void) { if (on_sub_thread()) { up_flush(); up_wait_zero(&g_up_pending); } return [g_q commandBuffer]; }
// Ask Metal to preserve per-encoder execution status for replay command buffers.
// A bare GPU-hang error only identifies the whole frame; with this option the
// error carries the label and debug signposts of the encoder that faulted.
static id<MTLCommandBuffer> replay_cb(void) {
    MTLCommandBufferDescriptor *d = [MTLCommandBufferDescriptor new];
    d.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
    return [g_q commandBufferWithDescriptor:d];
}
static void log_encoder_errors(NSError *error, uint64_t fence) {
    NSArray<id<MTLCommandBufferEncoderInfo>> *infos = error.userInfo[MTLCommandBufferEncoderInfoErrorKey];
    for (id<MTLCommandBufferEncoderInfo> info in infos) {
        if (info.errorState == MTLCommandEncoderErrorStateUnknown || info.errorState == MTLCommandEncoderErrorStateCompleted) continue;
        MLOG("【红】GPU 故障 encoder fence=%llu state=%ld label=%s signposts=%s",
             (unsigned long long)fence, (long)info.errorState,
             (info.label ?: @"-").UTF8String, (info.debugSignposts ?: @[]).description.UTF8String);
    }
}
static void log_function_errors(id<MTLCommandBuffer> cb, const char *kind, uint64_t fence) {
    static _Atomic unsigned reports;
    for (id<MTLFunctionLog> item in cb.logs) {
        if (atomic_fetch_add(&reports, 1) >= 64) break;
        id<MTLFunctionLogDebugLocation> loc = item.debugLocation;
        MLOG("[RED] Shader validation %s fence=%llu encoder=%s function=%s source=%s:%lu:%lu detail=%s", kind, (unsigned long long)fence, (item.encoderLabel ?: @"-").UTF8String, (loc.functionName ?: item.function.name ?: @"-").UTF8String, (loc.URL.path ?: @"-").UTF8String, (unsigned long)loc.line, (unsigned long)loc.column, item.description.UTF8String);
    }
}
static post_op *g_post; static int g_npost, g_postcap;
// One autorelease pool per submission: submissions run on a guest thread (long-lived, no run loop, never drains the implicit pool), so objects autoreleased in the Metal replay
// (command buffers, encoders, pass descriptors, etc.) are never released unless put in a pool. Measured on the character select screen of a real iOS device: the host heap grows by about 47 objects per frame, 0.22 MiB/s.
void *objc_autoreleasePoolPush(void); void objc_autoreleasePoolPop(void *);
static void *g_sub_pool;
static int g_ruler_truth = -1, g_ruler_pending;
static int tbdr_on(void);

void mtl_sub_begin(void) {
    mtl_init(); g_sub_serial++; g_rc_npend = 0; g_rc_skip = 0;
    if (tbdr_on() == 0 && g_ruler_pending) { g_ruler_truth = 0; g_ruler_pending = 0; MLOG("DM_NVN_RULER：切到被测模式"); }
    defer_drain();
    defer_reset();
    warm_claim();
    if (g_cb) MDIE("提交重入");
    defer_submission_begin_state();
    g_sub_thread = pthread_self(); atomic_store(&g_sub_thread_set, 1);
    atomic_store(&g_last_commit_error, 0);
    g_sub_pool = objc_autoreleasePoolPush();
    g_cb = replay_cb();
    g_defer_cut_active = 1;
    if (!g_sub_samplers) g_sub_samplers = [NSMutableSet set];
    [g_sub_samplers removeAllObjects];
    [g_sub_sample_notes setString:@""];
    // Do not wait for the GPU mid-frame (hot real device: with a fixed rotation of 3, when the GPU cannot keep up every frame blocks here, and the submit thread runs at low frequency after waking, instruction count −18% but CPU time +49%):
    // pick an allocator the GPU has finished with; if all are in use create a new one; only at the cap wait for the oldest one. How far ahead it runs is bounded by the wait where the game acquires the swapchain texture, same as the old interface.
    int alloc_i = -1; uint64_t done = g_done;
    for (int i = 0; i < g_alloc_n; i++) if (g_alloc_fence[i] <= done) { alloc_i = i; break; }
    if (alloc_i < 0 && g_alloc_n < ALLOC_MAX) { alloc_i = g_alloc_n++; g_alloc_grown++; }
    if (alloc_i < 0) { alloc_i = 0; for (int i = 1; i < g_alloc_n; i++) if (g_alloc_fence[i] < g_alloc_fence[alloc_i]) alloc_i = i; g_alloc_waits++; mtl_wait(g_alloc_fence[alloc_i], UINT64_MAX); }
    g_alloc_cur = alloc_i;
    g_commit_blocks = [NSMutableArray array];
    g_sub_argument_checks = nil;
    // The overflow buffer created when the previous submission ran out of slots (see g_vis_n >= VIS_SLOTS): retire it. It used to be just cleared here and stayed in the residency set for good (from d2r-mobile 2eb575877)
    if (g_vis && !vis_in_ring(g_vis)) defer_obj_release(g_vis);
    g_npost = 0; g_vis = nil; g_vis_n = 0; g_vis_cur = -1; g_npend = 0; g_pend_over = 0;
    zskip_clear(); g_zskip_count = 0;
    int pick = -1;
    for (int i = 0; i < g_vis_ring_n; i++) if (g_vis_ring[i].fence <= g_done) { pick = i; break; }
    if (pick < 0 && g_vis_ring_n < 4) pick = g_vis_ring_n++;
    if (pick < 0) { uint64_t oldest = UINT64_MAX; for (int i = 0; i < 4; i++) if (g_vis_ring[i].fence < oldest) { oldest = g_vis_ring[i].fence; pick = i; } if (oldest > g_done) mtl_wait(oldest, UINT64_MAX); }
    g_vis_ring_i = pick; g_vis_ring[pick].fence = UINT64_MAX;
    if (!g_vis_ring[pick].buf) { g_vis_ring[pick].buf = [g_dev newBufferWithLength:VIS_SLOTS * 8 options:MTLResourceStorageModeShared]; res_add(g_vis_ring[pick].buf); }
    g_vis = g_vis_ring[pick].buf;
    void pg_sub_begin_ext(uint64_t fence); pg_sub_begin_ext(g_fence + 1);
    void arena_begin(void); arena_begin();
}

static void rp_end(void);
extern int g_frz_on; static int g_frz_state;   // World freeze: the state latched for this frame (0 normal / 1 full frame / 2 frozen frame), see the note above frz_op
// DM_NVN_PASSGPU_SERIAL=1 (for measurement): before creating each encoder on the main command buffer, encode 'signal n+1 → wait for signal n+1' - the signal is only raised once all prior work has completed,
// so the following encoder starts only after the previous one has fully finished; on TBDR passes no longer overlap in the pipeline, and the per-pass start-to-end span of DM_NVN_PASSGPU is that pass's exclusive time (the whole-frame total is inflated; look only at relative cost)
static void ser_gate(void) {
    static int on = -1; static id<MTLEvent> ev; static uint64_t n;
    if (on < 0) { on = getenv("DM_NVN_PASSGPU_SERIAL") != NULL; if (on) { ev = [g_dev newEvent]; MLOG("DM_NVN_PASSGPU_SERIAL：通道串行化（测量用，耗时偏大）"); } }
    if (!on) return;
    (void)ev; (void)n;
}
// Guest memory pools can expose the same allocation through different Metal
// buffer/texture objects. Automatic tracking cannot infer all such aliases.
// Keep the original backend's full pass ordering, including texture-cache
// visibility, with a queue-local fence. Z-A v47 faulted in a single-sample
// 7x4 postprocess pass while OP_BARRIER only set an otherwise-unused flag.
static id<MTLFence> g_pass_order_fence;
static __unsafe_unretained id g_pass_order_encoder;
static int g_pass_order_kind, g_pass_order_valid;
static uint64_t g_pass_order_waits, g_explicit_barriers;
static void legacy_order_begin(id e, int kind, id<MTLCommandBuffer> cb) {
    if (!e || !on_sub_thread() || cb != g_cb) return;
    if (!g_pass_order_fence) { g_pass_order_fence = [g_dev newFence]; MLOG("legacy Metal pass ordering: explicit render/compute/blit fence for aliased guest resources"); }
    if (g_pass_order_valid) {
        if (kind == 1) [(id<MTLRenderCommandEncoder>)e waitForFence:g_pass_order_fence beforeStages:MTLRenderStageVertex];
        else if (kind == 2) [(id<MTLComputeCommandEncoder>)e waitForFence:g_pass_order_fence];
        else [(id<MTLBlitCommandEncoder>)e waitForFence:g_pass_order_fence];
        g_pass_order_waits++;
    }
    g_pass_order_encoder = e; g_pass_order_kind = kind;
}
static inline void m4_end(id e) {
    if (e && on_sub_thread() && e == g_pass_order_encoder) {
        if (g_pass_order_kind == 1) [(id<MTLRenderCommandEncoder>)e updateFence:g_pass_order_fence afterStages:MTLRenderStageFragment];
        else if (g_pass_order_kind == 2) [(id<MTLComputeCommandEncoder>)e updateFence:g_pass_order_fence];
        else [(id<MTLBlitCommandEncoder>)e updateFence:g_pass_order_fence];
        g_pass_order_valid = 1; g_pass_order_encoder = nil;
    }
    [e endEncoding];
}
// Explicit creation epochs prevent reused Objective-C encoder addresses from
// inheriting declarations. Flags are a union even when dedup is disabled.
#include "nvn_resource_usage.h"
static nvn_resource_usage_set g_legacy_use;
static uint64_t g_legacy_use_calls, g_legacy_use_skipped;
// Submit-thread-only encoder cache. Reset on creation, never keyed by an Objective-C address alone.
static struct {
    uint32_t valid, buffer_valid[2];
    uint32_t pso_valid, dss_valid;
    MTLViewport viewport; MTLScissorRect scissor;
    MTLCullMode cull; MTLWinding winding; MTLTriangleFillMode fill;
    float bias[3], blend[4]; uint32_t stencil[2];
    __unsafe_unretained id<MTLBuffer> buffer[2][32]; NSUInteger offset[2][32];
    __unsafe_unretained id<MTLRenderPipelineState> pso;
    __unsafe_unretained id<MTLDepthStencilState> dss;
} g_legacy_state;
static uint64_t g_legacy_state_calls, g_legacy_state_skips;
static uint64_t g_legacy_pso_calls, g_legacy_pso_skips;
static uint64_t g_legacy_dss_calls, g_legacy_dss_skips;
static int legacy_state_on(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_NVN_STATE_CACHE"); on = !e || strcmp(e, "0"); }
    return on;
}
static int legacy_pipeline_bind_cache_on(void) {
    static int on = -1;
    if (on < 0) {
        if (!legacy_state_on()) on = 0;
        else { const char *e = getenv("DM_NVN_PIPELINE_BIND_CACHE"); on = !e || strcmp(e, "0"); }
    }
    return on;
}
static int legacy_same(unsigned bit, void *saved, const void *value, size_t size) {
    if (legacy_state_on() && (g_legacy_state.valid & bit) && !memcmp(saved, value, size)) { g_legacy_state_skips++; return 1; }
    memcpy(saved, value, size); g_legacy_state.valid |= bit; g_legacy_state_calls++; return 0;
}
static void legacy_bind_buffer(id<MTLRenderCommandEncoder> e, int fragment, NSUInteger index, id<MTLBuffer> b, NSUInteger off) {
    int offset_only = 0;
    if (index < 32) {
        uint32_t bit = 1u << index;
        int same_buffer = legacy_state_on() && (g_legacy_state.buffer_valid[fragment] & bit) && g_legacy_state.buffer[fragment][index] == b;
        if (same_buffer && g_legacy_state.offset[fragment][index] == off) { g_legacy_state_skips++; return; }
        // Metal retains the first binding. Offset-only updates require that same
        // non-nil buffer on this encoder; enc_r_begin invalidates every slot.
        offset_only = same_buffer && b && g_buffer_offset_cache;
        g_legacy_state.buffer_valid[fragment] |= bit; g_legacy_state.buffer[fragment][index] = b; g_legacy_state.offset[fragment][index] = off;
    }
    g_legacy_state_calls++;
    if (offset_only) {
        g_buffer_offset_calls++;
        if (fragment) [e setFragmentBufferOffset:off atIndex:index]; else [e setVertexBufferOffset:off atIndex:index];
    } else if (fragment) [e setFragmentBuffer:b offset:off atIndex:index]; else [e setVertexBuffer:b offset:off atIndex:index];
}
static inline id<MTLRenderCommandEncoder> enc_r_begin(MTLRenderPassDescriptor *d) {
    memset(&g_legacy_state, 0, sizeof g_legacy_state);
    id<MTLRenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:d];
    if (e) nvn_resource_usage_begin(&g_legacy_use);
    legacy_order_begin(e, 1, g_cb); return e;
}
static void legacy_use_resource(id<MTLCommandEncoder> en, id<MTLResource> r, MTLResourceUsage usage, int compute) {
    if (!r) return;
    static int dedup = -1;
    if (dedup < 0) { const char *v = getenv("DM_NVN_USE_DEDUP"); dedup = !v || strcmp(v, "0"); }
    uint64_t combined; int skip;
    if (!nvn_resource_usage_add(&g_legacy_use, (uintptr_t)(__bridge void *)r, usage, dedup, &combined, &skip))
        MDIE("resource-usage union allocation failed; refusing an incomplete GPU declaration");
    if (skip) { g_legacy_use_skipped++; return; }
    usage = (MTLResourceUsage)combined;
    if (compute) [(id<MTLComputeCommandEncoder>)en useResource:r usage:usage];
    else [(id<MTLRenderCommandEncoder>)en useResource:r usage:usage];
    g_legacy_use_calls++;
}
void mtl_barrier_stats(uint64_t *skip, uint64_t *full) { *skip = 0; *full = g_pass_order_waits; }
static int g_game_barrier;
static inline id<MTLRenderCommandEncoder> enc_r_begin_pass(MTLRenderPassDescriptor *d) { return enc_r_begin(d); }
static inline void enc_r_end_pass(id e) { m4_end(e);
}
static inline void enc_r_end(id e) { m4_end(e); }
static inline void enc_r_pso(id<MTLRenderCommandEncoder> e, id<MTLRenderPipelineState> p) {
    if (legacy_pipeline_bind_cache_on() && g_legacy_state.pso_valid && g_legacy_state.pso == p) {
        g_legacy_pso_skips++; g_legacy_state_skips++; return;
    }
    g_legacy_state.pso_valid = 1; g_legacy_state.pso = p;
    g_legacy_pso_calls++; g_legacy_state_calls++;
    [e setRenderPipelineState:p];
}
static inline void enc_r_dss(id<MTLRenderCommandEncoder> e, id<MTLDepthStencilState> s) {
    if (legacy_pipeline_bind_cache_on() && g_legacy_state.dss_valid && g_legacy_state.dss == s) {
        g_legacy_dss_skips++; g_legacy_state_skips++; return;
    }
    g_legacy_state.dss_valid = 1; g_legacy_state.dss = s;
    g_legacy_dss_calls++; g_legacy_state_calls++;
    [e setDepthStencilState:s];
}
static inline void enc_r_viewport(id<MTLRenderCommandEncoder> e, MTLViewport v) { if (!legacy_same(1, &g_legacy_state.viewport, &v, sizeof v)) [e setViewport:v]; }
static inline void enc_r_scissor(id<MTLRenderCommandEncoder> e, MTLScissorRect r) { if (!legacy_same(2, &g_legacy_state.scissor, &r, sizeof r)) [e setScissorRect:r]; }
static inline void enc_r_cull(id<MTLRenderCommandEncoder> e, MTLCullMode m) { if (!legacy_same(4, &g_legacy_state.cull, &m, sizeof m)) [e setCullMode:m]; }
static inline void enc_r_winding(id<MTLRenderCommandEncoder> e, MTLWinding w) { if (!legacy_same(8, &g_legacy_state.winding, &w, sizeof w)) [e setFrontFacingWinding:w]; }
static inline void enc_r_fill(id<MTLRenderCommandEncoder> e, MTLTriangleFillMode m) { if (!legacy_same(16, &g_legacy_state.fill, &m, sizeof m)) [e setTriangleFillMode:m]; }
static inline void enc_r_depth_bias(id<MTLRenderCommandEncoder> e, float u, float s, float c) { float v[3] = {u,s,c}; if (!legacy_same(32, g_legacy_state.bias, v, sizeof v)) [e setDepthBias:u slopeScale:s clamp:c]; }
static inline void enc_r_depth_clip(id<MTLRenderCommandEncoder> e, BOOL v) { [e setDepthClipMode:v ? MTLDepthClipModeClip : MTLDepthClipModeClamp]; }
static inline void enc_r_stencil_ref(id<MTLRenderCommandEncoder> e, uint32_t f, uint32_t b) {
    static int separate = -1; if (separate < 0) { const char *v = getenv("DM_NVN_SEPARATE_STENCIL"); separate = !v || strcmp(v, "0"); }
    if (!separate) b = f;
    uint32_t v[2] = {f,b}; if (!legacy_same(64, g_legacy_state.stencil, v, sizeof v)) [e setStencilFrontReferenceValue:f backReferenceValue:b];
}
static inline void enc_r_blend_color(id<MTLRenderCommandEncoder> e, float r, float g, float b, float a) { float v[4] = {r,g,b,a}; if (!legacy_same(128, g_legacy_state.blend, v, sizeof v)) [e setBlendColorRed:r green:g blue:b alpha:a]; }
static inline void enc_r_visibility(id<MTLRenderCommandEncoder> e, MTLVisibilityResultMode m, NSUInteger o) { [e setVisibilityResultMode:m offset:o]; }
static inline void enc_r_draw(id<MTLRenderCommandEncoder> e, MTLPrimitiveType p, NSUInteger v, NSUInteger n) { [e drawPrimitives:p vertexStart:v vertexCount:n]; }
static inline void enc_r_draw_instanced(id<MTLRenderCommandEncoder> e, MTLPrimitiveType p, NSUInteger v, NSUInteger n, NSUInteger i, NSUInteger b) { [e drawPrimitives:p vertexStart:v vertexCount:n instanceCount:i baseInstance:b]; }
static inline void enc_r_draw_indexed(id<MTLRenderCommandEncoder> e, MTLPrimitiveType p, NSUInteger n, MTLIndexType t, uint64_t gpu, NSUInteger len, NSUInteger i, NSInteger bv, NSUInteger bi) { (void)len; if (!n) return; NSUInteger off; id<MTLBuffer> b = legacy_buffer_at(gpu, &off); if (!b || off > b.length || n > (b.length - off) / (t == MTLIndexTypeUInt16 ? 2 : 4)) MDIE("【红】Invalid index buffer range"); [e drawIndexedPrimitives:p indexCount:n indexType:t indexBuffer:b indexBufferOffset:off instanceCount:i baseVertex:bv baseInstance:bi]; }
static inline void enc_r_draw_indirect(id<MTLRenderCommandEncoder> e, MTLPrimitiveType p, id<MTLBuffer> a, NSUInteger ao) {
    if (!a || ao > a.length || a.length - ao < sizeof(MTLDrawPrimitivesIndirectArguments)) MDIE("[RED] Invalid indirect argument buffer range");
    legacy_use_resource(e, a, MTLResourceUsageRead, 0);
    [e drawPrimitives:p indirectBuffer:a indirectBufferOffset:ao];
}
static inline void enc_r_draw_indexed_indirect(id<MTLRenderCommandEncoder> e, MTLPrimitiveType p, MTLIndexType t,
                                                id<MTLBuffer> ib, NSUInteger io, NSUInteger il,
                                                id<MTLBuffer> a, NSUInteger ao) {
    if (!ib || io > ib.length || il > ib.length - io || !a || ao > a.length || a.length - ao < sizeof(MTLDrawIndexedPrimitivesIndirectArguments))
        MDIE("[RED] Invalid indexed-indirect buffer range");
    legacy_use_resource(e, ib, MTLResourceUsageRead, 0);
    legacy_use_resource(e, a, MTLResourceUsageRead, 0);
    [e drawIndexedPrimitives:p indexType:t indexBuffer:ib indexBufferOffset:io indirectBuffer:a indirectBufferOffset:ao];
}
static inline id<MTLComputeCommandEncoder> enc_c_begin(MTLComputePassDescriptor *d) {
    id<MTLComputeCommandEncoder> e = d ? [g_cb computeCommandEncoderWithDescriptor:d] : [g_cb computeCommandEncoder];
    if (e) nvn_resource_usage_begin(&g_legacy_use);
    legacy_order_begin(e, 2, g_cb); return e;
}
static inline id<MTLComputeCommandEncoder> enc_c_begin_concurrent(id<MTLCommandBuffer> cb) {
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
    if (e) nvn_resource_usage_begin(&g_legacy_use);
    return e;
}
static inline void enc_c_end(id e) { m4_end(e); }
static inline void enc_c_pso(id e, id<MTLComputePipelineState> p) { [e setComputePipelineState:p]; }
static inline void enc_c_args(id e, uint64_t gpu, NSUInteger i) { NSUInteger off; id<MTLBuffer> b = legacy_buffer_at(gpu, &off); if (gpu && !b) MDIE("【红】Unregistered compute argument address"); [e setBuffer:b offset:off atIndex:i]; }
static inline void enc_c_dispatch(id e, MTLSize n, MTLSize t) { [e dispatchThreadgroups:n threadsPerThreadgroup:t]; }
static inline id enc_b_begin(id cb) { id e = [cb blitCommandEncoder]; legacy_order_begin(e, 3, cb); return e; }
static inline void enc_b_end(id e) { m4_end(e); }
static inline void enc_b_copy_buffer(id e, id<MTLBuffer> s, NSUInteger so, id<MTLBuffer> d, NSUInteger doff, NSUInteger n) { [e copyFromBuffer:s sourceOffset:so toBuffer:d destinationOffset:doff size:n]; }
static inline void enc_b_copy_buffer_texture(id e, id<MTLBuffer> s, NSUInteger so, NSUInteger row, NSUInteger img, MTLSize z, id<MTLTexture> d, NSUInteger sl, NSUInteger lv, MTLOrigin o) { [e copyFromBuffer:s sourceOffset:so sourceBytesPerRow:row sourceBytesPerImage:img sourceSize:z toTexture:d destinationSlice:sl destinationLevel:lv destinationOrigin:o]; }
static inline void enc_b_copy_texture_buffer(id e, id<MTLTexture> s, NSUInteger sl, NSUInteger lv, MTLOrigin o, MTLSize z, id<MTLBuffer> d, NSUInteger doff, NSUInteger row, NSUInteger img) { [e copyFromTexture:s sourceSlice:sl sourceLevel:lv sourceOrigin:o sourceSize:z toBuffer:d destinationOffset:doff destinationBytesPerRow:row destinationBytesPerImage:img]; }
static inline void enc_b_copy_texture(id e, id<MTLTexture> s, NSUInteger sl, NSUInteger lv, MTLOrigin so, MTLSize z, id<MTLTexture> d, NSUInteger dl, NSUInteger dv, MTLOrigin doo) { [e copyFromTexture:s sourceSlice:sl sourceLevel:lv sourceOrigin:so sourceSize:z toTexture:d destinationSlice:dl destinationLevel:dv destinationOrigin:doo]; }
static void blit_note(void);   // Per-pass dump: a copy / buffer update ended the render pass (defined after PD)
// Traditional blits are ordered by the API. Keep range accounting for the
// existing diagnostics; it does not emit Metal 4 barrier messages.
enum { BW_MAX = 64 };
static uint64_t g_bw_lo[BW_MAX], g_bw_hi[BW_MAX]; static int g_bw_n, g_blit_ops; static uint64_t g_blit_barriers;
static uint64_t g_blit_splits;
uint64_t mtl_blit_splits(void) { return g_blit_splits; }
static int enc_open(void);
static id<MTLBlitCommandEncoder> blit_raw(void) { blit_note(); if (enc_open()) g_blit_splits++; rp_end(); if (!g_blit) { ser_gate(); g_blit = enc_b_begin(g_cb);  g_bw_n = 0; g_blit_ops = 0; } return g_blit; }
static void blit_barrier(void) {
    g_bw_n = 0; g_blit_ops = 0; g_blit_barriers++;
}
uint64_t mtl_blit_barriers(void) { return g_blit_barriers; }
static int bw_hit(uint64_t lo, uint64_t hi) { for (int i = 0; i < g_bw_n; i++) if (lo < g_bw_hi[i] && g_bw_lo[i] < hi) return 1; return 0; }
// buffer → buffer / fill: reads [rlo, rhi) (0, 0 when nothing is read), writes [wlo, whi)
static id<MTLBlitCommandEncoder> blit_buf(uint64_t rlo, uint64_t rhi, uint64_t wlo, uint64_t whi) {
    id<MTLBlitCommandEncoder> e = blit_raw();
    if (g_blit_ops && (g_bw_n == BW_MAX || (rhi > rlo && bw_hit(rlo, rhi)) || bw_hit(wlo, whi))) blit_barrier();
    g_bw_lo[g_bw_n] = wlo; g_bw_hi[g_bw_n++] = whi; g_blit_ops++;
    return e;
}
// texture copies / readbacks: barrier if anything came before, and later operations must wait for it too
static id<MTLBlitCommandEncoder> blit(void) {
    id<MTLBlitCommandEncoder> e = blit_raw();
    if (g_blit_ops) blit_barrier();
    g_bw_lo[0] = 0; g_bw_hi[0] = ~0ull; g_bw_n = 1; g_blit_ops = 1;   // record as "everything written": the next operation always gets a barrier
    return e;
}

void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    pend_add(dst, n);
    void *sb, *db; uint64_t so, dof;
    if (!mtl_addr(src, n, &sb, &so) || !mtl_addr(dst, n, &db, &dof))
        MDIE("缓冲拷贝地址不在任何内存池内 src=%#llx dst=%#llx n=%#llx", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n);
    enc_b_copy_buffer(blit_buf(src, src + n, dst, dst + n), (__bridge id<MTLBuffer>)sb, so, (__bridge id<MTLBuffer>)db, dof, n);
}

// Inline uniform update (UpdateUniformBuffer): the data is copied into the host's own shared ring at encode time, then copied in order to the destination.
// Previously it was read from command memory only when the GPU executed - if the game recycles command memory before the GPU runs, it reads the bytes of the next batch of commands (SMBW's fountain water wave simulation got the wrong coefficients and diverged to ±65504).
static id<MTLBuffer> g_ubs_buf; static size_t g_ubs_off, g_ubs_size;
// Inline updates inside an open render pass are deferred instead of ending the pass. Metal cannot copy into a buffer inside a render pass, so each update used to end it: all attachments stored,
// reloaded by the next pass, new encoders (Metroid Dread about 246 times a frame, Tears of the Kingdom about 50). NVN applies the update in command order, so the draws after it in the pass must
// read the new data: a draw whose uniform binding overlaps a deferred update reads a shadow made only of the updates deferred in this pass, latest first per byte, and only when they cover the
// whole bound range (then the shadow is exactly what the GPU would read, whatever earlier frames still have in flight). The real copies are encoded in order when the pass ends (rp_end), so later
// passes and the CPU see them as before. The pass still ends before a draw that would need more: a binding the deferred updates cover only in part, or a read of the deferred data through a vertex,
// index, indirect or storage buffer (ubd_conflict). Off in TBDR mode (it reorders passes). DM_NVN_UB_DEFER=0 turns it off
static size_t arena_alloc(size_t n, __strong id<MTLBuffer> *buf); static inline void args_dirty_all(void);
typedef struct { uint64_t lo, hi, gpu; size_t off; __unsafe_unretained id<MTLBuffer> buf; } ubd_ent;   // data at buf + off (the ubs ring; a ring block replaced meanwhile lives until this submission completes)
enum { UBD_MAX = 512 };
static ubd_ent g_ubd[UBD_MAX]; static int g_nubd;
static uint64_t g_ubd_deferred, g_ubd_conflicts;   // updates deferred, passes ended before a draw that needed more (the 驻留集 line)
static int ubd_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_UB_DEFER"); v = !(e && !strcmp(e, "0")); if (!v) MLOG("DM_NVN_UB_DEFER=0：通道内的 uniform 更新照旧结束通道"); } return v && !tbdr_on(); }
void mtl_ubd_stats(uint64_t *deferred, uint64_t *conflicts) { *deferred = g_ubd_deferred; *conflicts = g_ubd_conflicts; }
static int ubd_hits(uint64_t lo, uint64_t hi) { for (int i = 0; i < g_nubd; i++) if (lo < g_ubd[i].hi && g_ubd[i].lo < hi) return 1; return 0; }
// Whether the deferred updates cover [lo, hi) entirely (any order of overlap): the union of their ranges clipped to it, swept from lo
static int ubd_covers(uint64_t lo, uint64_t hi) {
    for (uint64_t at = lo; at < hi;) { uint64_t next = at;
        for (int i = 0; i < g_nubd; i++) if (g_ubd[i].lo <= at && g_ubd[i].hi > next) next = g_ubd[i].hi;
        if (next == at) return 0; at = next; }
    return 1;
}
// GPU address of the shadow of uniform range [lo, hi) (ubd_covers holds): one deferred update holding all of it and suitably aligned is used in place; otherwise the updates are laid into the arena in order
static uint64_t ubd_shadow(uint64_t lo, uint64_t hi) {
    for (int i = g_nubd - 1; i >= 0; i--) { const ubd_ent *e = &g_ubd[i];
        if (e->lo <= lo && hi <= e->hi) { int later = 0; for (int j = i + 1; j < g_nubd; j++) later |= lo < g_ubd[j].hi && g_ubd[j].lo < hi;
            uint64_t g = e->gpu + (lo - e->lo); if (!later && !(g & 15)) return g; break; } }
    id<MTLBuffer> ab; size_t o = arena_alloc((size_t)(hi - lo), &ab); uint8_t *d = (uint8_t *)ab.contents + o;
    for (int i = 0; i < g_nubd; i++) { const ubd_ent *e = &g_ubd[i]; uint64_t a = e->lo > lo ? e->lo : lo, b = e->hi < hi ? e->hi : hi;
        if (a < b) memcpy(d + (a - lo), (const uint8_t *)e->buf.contents + e->off + (a - e->lo), (size_t)(b - a)); }
    return legacy_buffer_gpu(ab) + o;
}
// Encode the deferred copies in order (rp_end, once the pass has ended)
static void ubd_flush(void) {
    int n = g_nubd; g_nubd = 0;
    for (int i = 0; i < n; i++) { const ubd_ent *e = &g_ubd[i]; void *db; uint64_t dof;
        if (!mtl_addr(e->lo, e->hi - e->lo, &db, &dof)) continue;
        enc_b_copy_buffer(blit_buf(0, 0, e->lo, e->hi), e->buf, e->off, (__bridge id<MTLBuffer>)db, dof, e->hi - e->lo); }
}
// The bytes [lo, lo+n) as the deferred updates leave them (ubd_covers holds): for bindless handles read from uniform memory (ubo_handle)
static void ubd_read(uint64_t lo, size_t n, void *out) {
    for (int i = 0; i < g_nubd; i++) { const ubd_ent *e = &g_ubd[i]; uint64_t a = e->lo > lo ? e->lo : lo, b = e->hi < lo + n ? e->hi : lo + n;
        if (a < b) memcpy((uint8_t *)out + (a - lo), (const uint8_t *)e->buf.contents + e->off + (a - e->lo), (size_t)(b - a)); }
}
void mtl_sub_update(const void *data, uint64_t dst, uint64_t n) {
    if (!n) return;
    pend_add(dst, n);
    void *db; uint64_t dof;
    if (!mtl_addr(dst, n, &db, &dof)) MDIE("uniform 更新目标不在任何内存池内 dst=%#llx n=%#llx", (unsigned long long)dst, (unsigned long long)n);
    int defer = enc_open() && g_nubd < UBD_MAX && ubd_on();
    size_t need = (n + 255u) & ~(size_t)255u;   // 256-aligned slots: a deferred update is read in place by draws as a uniform buffer
    if (!g_ubs_buf || g_ubs_off + need > g_ubs_size) {
        if (g_ubs_buf) defer_obj_release(g_ubs_buf);   // copies already encoded still reference the old block: release after this submission completes
        g_ubs_size = MAX((size_t)1 << 20, need);
        g_ubs_buf = [g_dev newBufferWithLength:g_ubs_size options:MTLResourceStorageModeShared];
        if (!g_ubs_buf) MDIE("uniform 更新环分配失败 %#zx", g_ubs_size);
        res_add(g_ubs_buf); g_ubs_off = 0;
    }
    memcpy((uint8_t *)g_ubs_buf.contents + g_ubs_off, data, n);
    if (defer) { g_ubd[g_nubd++] = (ubd_ent){dst, dst + n, legacy_buffer_gpu(g_ubs_buf) + g_ubs_off, g_ubs_off, g_ubs_buf}; g_ubd_deferred++; args_dirty_all(); }   // see ubd_shadow
    else enc_b_copy_buffer(blit_buf(0, 0, dst, dst + n), g_ubs_buf, g_ubs_off, (__bridge id<MTLBuffer>)db, dof, n);
    g_ubs_off += need;
}

// In-order buffer fill within the submission: all four bytes equal → fillBuffer; otherwise copied in chunks from a 64 KiB pattern buffer
void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value) {
    if (!n) return;
    pend_add(dst, n);
    void *db; uint64_t dof;
    if (!mtl_addr(dst, n, &db, &dof)) MDIE("缓冲填充地址不在任何内存池内 dst=%#llx n=%#llx", (unsigned long long)dst, (unsigned long long)n);
    uint8_t b0 = (uint8_t)value;
    if (value == (uint32_t)b0 * 0x01010101u) { [blit_buf(0, 0, dst, dst + n) fillBuffer:(__bridge id<MTLBuffer>)db range:NSMakeRange(dof, n) value:b0]; return; }
    enum { PAT = 0x10000 }; static __strong id<MTLBuffer> pat; static uint32_t pat_v; static int pat_ok;
    if (!pat_ok || pat_v != value) { uint32_t *tmp = malloc(PAT); for (unsigned i = 0; i < PAT / 4; i++) tmp[i] = value;
        pat = [g_dev newBufferWithBytes:tmp length:PAT options:MTLResourceStorageModeShared]; free(tmp); pat_v = value; pat_ok = 1; res_add(pat); }
    for (uint64_t off = 0; off < n; off += PAT) { uint64_t k = n - off < PAT ? n - off : PAT; enc_b_copy_buffer(blit_buf(0, 0, dst + off, dst + off + k), pat, 0, (__bridge id<MTLBuffer>)db, dof + off, k); }
}

static void post(post_op o) {
    if (g_npost == g_postcap) { g_postcap = g_postcap ? g_postcap * 2 : 64; g_post = realloc(g_post, g_postcap * sizeof *g_post); }
    g_post[g_npost++] = o;
}
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){1, val, mode, v, 0, 0, NULL}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) { post((post_op){2, NULL, 0, 0, addr, type, NULL}); }
void mtl_counter_stats(uint64_t rep[32], uint64_t rst[32]) { memcpy(rep, g_cnt_report, sizeof g_cnt_report); memcpy(rst, g_cnt_reset, sizeof g_cnt_reset); }

// Counter timestamps (19.2 MHz) are taken from the guest clock (hle_os.c guest_ns, the same source as svc GetSystemTick / cntpct): on hardware GPU timestamps and the CPU counter share one timebase, and games compare them.
// They used to come from CLOCK_MONOTONIC, which keeps running while the Mac sleeps while the guest clock (mach_absolute_time) does not, so the two differed by the sleep time; once timestamps were converted to ns, Mario halved its frame rate. In graphics unit tests guest_ns is a weak definition returning 0; fall back to the monotonic clock
uint64_t guest_ns(void);
static uint64_t gpu_ticks(void) { uint64_t g = guest_ns(); if (!g) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); g = (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; } return g * 12 / 625; }

// ---------- Boot instrumentation (DM_BOOTPROF=1): metallib loading, pipeline creation (both happen synchronously during replay on the submit thread), texture upload, first-frame present ----------
static uint64_t bp_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static uint64_t g_bp_t0;   // Time of process start (when this module is loaded)
__attribute__((constructor)) static void bp_t0(void) { g_bp_t0 = bp_now(); }
static int bootprof(void) { static int v = -1; if (v < 0) v = getenv("DM_BOOTPROF") != NULL; return v; }
static uint64_t g_bp_lib_n, g_bp_lib_ns, g_bp_pso_n, g_bp_pso_ns, g_bp_up_bytes, g_bp_first_present;
static uint64_t g_up_total_ns;   // Cumulative upload time (same source as the per-frame @upload_ms)
// ---------- Per-frame GPU time aggregation ----------
#define GRING 1024
static struct { _Atomic uint64_t frame, ns, t0, t1; _Atomic int pending; } g_gpu[GRING];   // t0 / t1: the frame's earliest GPUStart / latest GPUEnd (mach absolute time, ns)
static _Atomic uint64_t g_cur_frame;
// ---------- Same-process alternating measurement (DM_NVN_UB, off by default: without the environment variable the code below only adds one 'is it enabled' check) ----------
// On a hot real device the system keeps adjusting power / frequency, and segment comparison (segments A and B each run for a minute) cannot resolve differences of <10%; within the same run, toggle by phase in turn and take differences of adjacent phase pairs, so the drift cancels.
// DM_NVN_UB="name=item,item;name=item", one phase every DM_NVN_UB_N frames (default 180, minimum 60): even phases = baseline (nothing changed), odd phases take one category in turn. Each item of a category:
//   @set:<global variable name>=<value>   on entering the phase set this int global variable to the value, on leaving restore it (address found by symbol name; switches to be used on a real device must have a line added to the registration table below)
//   <fragment shader key prefix> or *   the phase drops all draws of these fragment shaders (the picture will be wrong; it only measures 'how much this kind of draw is worth'; a nearly idle category needs a round of its own)
// Readings: each phase discards its first 45 frames (transition + present lag); for the remaining frames, the mean of the actual present interval (same source as perf.txt) and the frame counts in the three buckets 16.7 / 25 / ≥33 ms, one log line 「UB 上屏」(UB present).
// Table output: $DM_ROOT/run/ruler/ub_table.py; real-device script: $DM_ROOT/run/ruler/ub_alt.sh.
static int g_ub_n = -1, g_ub_N = 180; static _Atomic int g_ub_cur = -1; static char g_ub_name[16][16], g_ub_keys[16][16][48]; static int g_ub_nk[16];
static _Atomic int g_ubp_on, g_ubp_c[4]; static _Atomic uint64_t g_ubp_sum_us;
void mtl_ub_present(double iv) {   // Called once per frame by the present thread: actual present interval in ms (−1 = dropped, 0 = no reading)
    if (!atomic_load(&g_ubp_on) || iv == 0) return;
    if (iv < 0) { atomic_fetch_add(&g_ubp_c[3], 1); return; }
    atomic_fetch_add(&g_ubp_c[iv <= 20.8 ? 0 : iv <= 29.2 ? 1 : 2], 1); atomic_fetch_add(&g_ubp_sum_us, (uint64_t)(iv * 1000));
}
// Registered runtime switches: name → address. Main-program symbols of an iOS release build are not in the dynamic symbol table and dlsym cannot find them (in the first real-device round all four categories were 「找不到全局变量」(global variable not found) and the whole round was wasted),
// so a switch to be measured on a real device must have a line added here; on Mac / in unit tests unregistered ones are still looked up by symbol name.
static int *ub_var(const char *nm) {
    extern int g_bloom_q, g_dm_present_link;
    static const struct { const char *n; int *p; } t[] = { {"g_bloom_q", &g_bloom_q}, {"g_dm_present_link", &g_dm_present_link}, {"g_frz_on", &g_frz_on} };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(nm, t[i].n)) return t[i].p;
    return dlsym(RTLD_DEFAULT, nm);
}
static void ub_frame(uint64_t f) {
    if (g_ub_n < 0) { g_ub_n = 0; const char *e = getenv("DM_NVN_UB"), *n = getenv("DM_NVN_UB_N"); if (n && atoi(n) >= 60) g_ub_N = atoi(n);
        for (const char *q = e; q && *q && g_ub_n < 16; ) { const char *eq = strchr(q, '='), *end = strchr(q, ';'); if (!end) end = q + strlen(q); if (!eq || eq > end) break;
            int c = g_ub_n++; size_t l = (size_t)(eq - q); if (l > 15) l = 15; memcpy(g_ub_name[c], q, l);
            for (const char *k = eq + 1; k < end && g_ub_nk[c] < 16; ) { size_t kl = 0; while (k + kl < end && k[kl] != ',') kl++; size_t cl = kl > 47 ? 47 : kl; memcpy(g_ub_keys[c][g_ub_nk[c]++], k, cl); k += kl; if (k < end) k++; }
            q = *end ? end + 1 : end; }
        if (g_ub_n) MLOG("DM_NVN_UB：%d 类，每相 %d 帧（同进程交替测量；丢绘制的相画面不对）", g_ub_n, g_ub_N); }
    if (!g_ub_n) return;
    uint64_t ph = f / (uint64_t)g_ub_N, r = f % (uint64_t)g_ub_N;
    if (r == 45) { for (int i = 0; i < 4; i++) atomic_store(&g_ubp_c[i], 0); atomic_store(&g_ubp_sum_us, 0); atomic_store(&g_ubp_on, 1); }
    if (r != 0) return;
    if (f && atomic_load(&g_ubp_on)) { atomic_store(&g_ubp_on, 0); uint64_t pp = ph - 1; int pc = (pp & 1) ? (int)((pp / 2) % (uint64_t)g_ub_n) : -1;
        int c0 = atomic_load(&g_ubp_c[0]), c1 = atomic_load(&g_ubp_c[1]), c2 = atomic_load(&g_ubp_c[2]), c3 = atomic_load(&g_ubp_c[3]), n_ = c0 + c1 + c2;
        MLOG("UB 上屏 %llu %s 帧 %d 均 %.3f ms 2拍 %d 3拍 %d 4拍以上 %d 丢弃 %d", (unsigned long long)pp, pc < 0 ? "base" : g_ub_name[pc], n_, n_ ? atomic_load(&g_ubp_sum_us) / 1000.0 / n_ : 0.0, c0, c1, c2, c3); }
    int nc = (ph & 1) ? (int)((ph / 2) % (uint64_t)g_ub_n) : -1;
    static int *sp[16]; static int sv[16], ns;   // Variables set by the previous phase: restore first
    for (int i = 0; i < ns; i++) *sp[i] = sv[i]; ns = 0;
    for (int i = 0; nc >= 0 && i < g_ub_nk[nc] && ns < 16; i++) { const char *k = g_ub_keys[nc][i]; if (strncmp(k, "@set:", 5)) continue;
        char nm[48]; const char *eq = strchr(k, '='); if (!eq) continue; size_t l = (size_t)(eq - (k + 5)); if (l >= sizeof nm) continue; memcpy(nm, k + 5, l); nm[l] = 0;
        int *a_ = ub_var(nm); static int bad, ok;
        if (!a_) { if (bad++ < 8) MLOG("【红】DM_NVN_UB：找不到全局变量 %s（真机上要先在 ub_var 的表里登记）", nm); continue; }
        sp[ns] = a_; sv[ns] = *a_; ns++; *a_ = atoi(eq + 1); if (ok++ < 8) MLOG("DM_NVN_UB：%s = %d（原值 %d）", nm, atoi(eq + 1), sv[ns - 1]); }
    atomic_store(&g_ub_cur, nc);
}
static int ub_skip(const char *key) {   // Whether this phase should drop draws of this fragment shader
    int uc = atomic_load(&g_ub_cur); if (uc < 0) return 0;
    for (int i = 0; i < g_ub_nk[uc]; i++) { const char *k = g_ub_keys[uc][i]; if (k[0] == '@') continue; if (k[0] == '*' || !strncmp(key, k, strlen(k))) return 1; }
    return 0;
}
void mtl_set_frame(uint64_t f) { atomic_store(&g_cur_frame, f); if (g_ub_n) ub_frame(f); }
// GPU timing registration: gpu_tag_begin(frame) before commit, gpu_tag_done(frame, GPU start, GPU end) on completion (seconds on the CACurrentMediaTime base).
// Old-queue command buffers go through gpu_tag_f; the Metal 4 main submission backfills in the feedback of mtl_sub_commit (when switching to Metal 4 all the call sites were lost and the GPU column of the 「计时」(timing) line became empty)
static void gpu_tag_begin(uint64_t f) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f) { atomic_store(&g_gpu[k].frame, f); atomic_store(&g_gpu[k].ns, 0); atomic_store(&g_gpu[k].pending, 0); atomic_store(&g_gpu[k].t0, UINT64_MAX); atomic_store(&g_gpu[k].t1, 0); }
    atomic_fetch_add(&g_gpu[k].pending, 1);
}
static void gpu_tag_done(uint64_t f, double g0, double g1) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f) return;
    double d = g1 - g0;
    if (d > 0) {
        atomic_fetch_add(&g_gpu[k].ns, (uint64_t)(d * 1e9));
        uint64_t s0 = (uint64_t)(g0 * 1e9), s1 = (uint64_t)(g1 * 1e9), v;
        for (v = atomic_load(&g_gpu[k].t0); s0 < v && !atomic_compare_exchange_weak(&g_gpu[k].t0, &v, s0);) {}
        for (v = atomic_load(&g_gpu[k].t1); s1 > v && !atomic_compare_exchange_weak(&g_gpu[k].t1, &v, s1);) {}
    }
    atomic_fetch_sub(&g_gpu[k].pending, 1);
}
static void gpu_tag_f(id<MTLCommandBuffer> cb, uint64_t f) { gpu_tag_begin(f); [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { gpu_tag_done(f, c.GPUStartTime, c.GPUEndTime); }]; }
void gpu_tag(id<MTLCommandBuffer> cb) { gpu_tag_f(cb, atomic_load(&g_cur_frame)); }
double mtl_gpu_ms(uint64_t f) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f || atomic_load(&g_gpu[k].pending)) return -1;
    return atomic_load(&g_gpu[k].ns) / 1e6;
}
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f || atomic_load(&g_gpu[k].pending)) return 0;
    *t0 = atomic_load(&g_gpu[k].t0); *t1 = atomic_load(&g_gpu[k].t1);
    return *t1 >= *t0 && *t1;
}
static void rp_flush(void);
static const char *cctx_snap(void);   // What is in this submission (printed into the failure line on error; defined before mtl_exec)
uint64_t mtl_sub_commit(void) {
    defer_fallback();   // Deferred merged pass: at the end of the submission there are still P draws not replayed (the normal path never gets here)
    rp_flush();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    up_flush();
    uint64_t fence = defer_submission_prepare_commit();
    if (g_vis_ring_i >= 0) g_vis_ring[g_vis_ring_i].fence = fence;
    // CPU-visible effects after completion: copy the op table into the callback
    int n = g_npost; post_op *ops = n ? malloc(n * sizeof *ops) : NULL;
    if (n) memcpy(ops, g_post, n * sizeof *ops);
    NSArray<dispatch_block_t> *commit_blocks = [g_commit_blocks copy];
    __block NSArray<id<MTLSamplerState>> *sampler_refs = g_sub_samplers.allObjects;

    [g_cb addCompletedHandler:^(id<MTLCommandBuffer> f) { (void)f; for (dispatch_block_t block in commit_blocks) block(); sampler_refs = nil; }];
    uint64_t gpu_f = atomic_load(&g_cur_frame); gpu_tag_begin(gpu_f);
    NSString *cctx = @(cctx_snap());
    NSString *sample_notes = [g_sub_sample_notes copy];
    NSArray<dispatch_block_t> *argument_checks = [g_sub_argument_checks copy];
    [g_cb addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        // The diagnostic reads run in this callback before g_done advances,
        // so an arena cannot be legitimately recycled before the comparison.
        for (dispatch_block_t check in argument_checks) check();
        log_function_errors(cb, "replay", fence);
        gpu_tag_done(gpu_f, cb.GPUStartTime, cb.GPUEndTime);
        if (cb.error && sample_notes.length) { static int notes; if (notes++ < 3) MLOG("[sample-trace] failed fence=%llu\n%s", (unsigned long long)fence, sample_notes.UTF8String); }
        if (cb.error) { atomic_store(&g_last_commit_error, 1); static int n, nv; int victim = [cb.error.description containsString:@"InnocentVictim"]; if (victim ? nv++ < 1 : n++ < 8) MLOG("【红】命令缓冲错误详情 fence=%llu：%s | GPU %.3f→%.3f", (unsigned long long)fence, cb.error.description.UTF8String, cb.GPUStartTime, cb.GPUEndTime); if (!victim) { MLOG("【红】出错那次提交的现场 fence=%llu：%s", (unsigned long long)fence, cctx.UTF8String); log_encoder_errors(cb.error, fence); } MLOG("【红】命令缓冲执行出错：%s", cb.error.localizedDescription.UTF8String); }
        for (int i = 0; i < n; i++) {
            if (ops[i].kind == 1) { if (ops[i].mode == 1) *ops[i].val += ops[i].v; else *ops[i].val = ops[i].v; }
            else if (ops[i].kind == 3) {   // Samples passed: the sum of all slots since the reset
                vis_list *l = ops[i].vis; uint64_t sum = 0;
                for (int k = 0; k < l->n; k++) { sum += ((const uint64_t *)((__bridge id<MTLBuffer>)l->s[k].buf).contents)[l->s[k].slot]; CFRelease(l->s[k].buf); }
                if (!sum && vis_force_visible()) sum = 1;
                free(l); uint64_t rec[2] = {sum, gpu_ticks()}; memcpy((void *)ops[i].addr, rec, sizeof rec);
            }
            else { uint64_t t = gpu_ticks(), rec[2] = {(ops[i].type == 0 || ops[i].type == 0x10) && !DM_GAME_RENDER_COND ? t : 0, t}; memcpy((void *)ops[i].addr, rec, sizeof rec); }
        }
        free(ops);
        pthread_mutex_lock(&g_mu); if (fence > g_done) g_done = fence; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
        defer_drain();
    }];
    void arena_commit(uint64_t fence); arena_commit(fence);
    res_commit();
    [g_cb encodeSignalEvent:g_ev value:fence];
    [g_cb commit];
    // Registration threads can now linearize after a real queue commit. Keep
    // active=true until this point so a concurrently retired replay view is
    // never assigned to an uncommitted/current fence by a stale observation.
    defer_submission_did_commit(fence);
    g_alloc_fence[g_alloc_cur] = fence;
    g_cb = nil;
    g_defer_cut_active = 0;
    g_commit_blocks = nil;
    g_sub_argument_checks = nil;
    objc_autoreleasePoolPop(g_sub_pool); g_sub_pool = NULL;
    return fence;
}

uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_last_commit_error(void) { return atomic_load(&g_last_commit_error); }

int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
    dl.tv_sec += ns / 1000000000ull; dl.tv_nsec = ns % 1000000000ull;
    pthread_mutex_lock(&g_mu);
    int r = 0;
    while (g_done < fence) if ((r = pthread_cond_timedwait(&g_cv, &g_mu, &dl))) break;
    int ok = g_done >= fence;
    pthread_mutex_unlock(&g_mu);
    return ok;
}

static void mtl_finish_gpu(void) {
    if (g_fence) mtl_wait(g_fence, UINT64_MAX);
    up_wait_zero(&g_up_pending);
}
void mtl_present_flush(void);
// nvnQueueFinish waits for the GPU work submitted so far, not for queued presents to reach the screen: with a window, the display link takes one present per tick, so waiting for them
// made every QueueFinish cost one or two display frames. Fire Emblem: Three Houses' loader submits a copy and calls QueueFinish for each resource it uploads, and its first loading screen
// took 78 s on the iPhone (5 s windowless on the Mac). A swapchain texture is not handed back before its previous present is processed (AcquireTexture waits for that ticket)
static NSString *pso_user_path(void) {
    const char *u = getenv("DM_PSO_USER"), *save = getenv("DM_SAVE");
    if (u && *u) return @(u);
    return save && *save ? [[@(save) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"pso_user.tsv"] : nil;
}
// The current generic host no longer ships the older fork's private PSO-learning
// helper. Keep its public counters inert while the Metal 3 backend is brought
// forward; on-demand pipeline creation remains functional.
typedef struct { uint64_t attempted, accepted, written, dropped, failed; } pso_learning_stats;
static inline pso_learning_stats pso_learning_snapshot(void) { return (pso_learning_stats){0}; }
static inline void pso_learning_drain_timeout(unsigned ms) { (void)ms; }
static inline void pso_learning_start(void) {}
static inline void pso_user_record(const char *fmt, ...) { (void)fmt; }

void mtl_pso_user_stats(uint64_t *attempted, uint64_t *accepted, uint64_t *written, uint64_t *dropped, uint64_t *failed) {
    pso_learning_stats s = pso_learning_snapshot();
    if (attempted) *attempted = s.attempted;
    if (accepted) *accepted = s.accepted;
    if (written) *written = s.written;
    if (dropped) *dropped = s.dropped;
    if (failed) *failed = s.failed;
}

void mtl_pso_user_drain(void) {
    pso_learning_drain_timeout(100);
}
void mtl_finish(void) { mtl_finish_gpu(); }
static void present_drop_for_suspend(void);
void mtl_drain(void) { pso_learning_drain_timeout(100); present_drop_for_suspend(); mtl_present_flush(); mtl_init(); id<MTLCommandBuffer> cb = q_cb(); [cb commit]; [cb waitUntilCompleted]; mtl_finish_gpu(); }

// ================= Render replay =================
#include "../../host/front.h"
static int g_red_once[128];
#define RED_ONCE(k, ...) do { if (!g_red_once[k]++) MLOG("[RED] " __VA_ARGS__); } while (0)
// NVN vertex streams 0–15 → Metal vertex buffer slots: 0–13 go to 16–29 (30 is the constant fallback buffer), 14 and 15 to 14 and 15. Metal has only the 31 slots 0–30; translated shaders use only buffer(0) (the argument buffer),
// and the host's own vertex functions read no vertex buffers. 13 Sentinels uses stream 15, and the old 16 + stream went out of range (Metal assertion: buffer index (31) must be < 31)
#define VB_SLOT(st) ((st) < 14 ? 16 + (st) : (st))

// ---------- Texture → MTLTexture ----------
static int fmt_compressed(int f) { return (f >= 0x41 && f <= 0x50) || (f >= 0x79 && f <= 0x94); }
static int fmt_depth(int f) { return f >= 0x31 && f <= 0x36; }
typedef struct { MTLPixelFormat a, b; } pfv_pair;
// Metal's BC enum names are SDK-gated to iOS 16.4 even though the numeric
// pixel-format ABI is stable. The 16.1 build never creates them: no_bc()
// routes every BC upload through nvn_bcdec first.
enum {
    DM_MTL_BC1 = 130, DM_MTL_BC1_SRGB = 131,
    DM_MTL_BC2 = 132, DM_MTL_BC2_SRGB = 133,
    DM_MTL_BC3 = 134, DM_MTL_BC3_SRGB = 135,
    DM_MTL_BC7 = 152, DM_MTL_BC7_SRGB = 153,
    DM_MTL_ASTC4_SRGB = 186, DM_MTL_ASTC4 = 204,
};
static const pfv_pair g_pfv_srgb[] = {
    {MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm_sRGB},
    {MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGRA8Unorm_sRGB},
    {(MTLPixelFormat)DM_MTL_BC1, (MTLPixelFormat)DM_MTL_BC1_SRGB},
    {(MTLPixelFormat)DM_MTL_BC2, (MTLPixelFormat)DM_MTL_BC2_SRGB},
    {(MTLPixelFormat)DM_MTL_BC3, (MTLPixelFormat)DM_MTL_BC3_SRGB},
    {(MTLPixelFormat)DM_MTL_BC7, (MTLPixelFormat)DM_MTL_BC7_SRGB},
    {(MTLPixelFormat)DM_MTL_ASTC4, (MTLPixelFormat)DM_MTL_ASTC4_SRGB},
};
static int tex_need_pfv(MTLPixelFormat base, MTLPixelFormat view) {
    if (base == view) return 0;
    for (size_t i = 0; i < sizeof g_pfv_srgb / sizeof *g_pfv_srgb; i++)
        if ((base == g_pfv_srgb[i].a && view == g_pfv_srgb[i].b) || (base == g_pfv_srgb[i].b && view == g_pfv_srgb[i].a)) return 0;
    return 1;
}
// Cache tables on the replay hot path always use nvn_hmap (C hash, keys not boxed); the table stores CFBridgingRetain'ed objects, CFRelease'd on replace / delete
static void hm_rel(void *v) { CFRelease(v); }
static __unsafe_unretained id hm_get_id(const nvn_hmap *m, const void *k) { return (__bridge id)nvn_hm_get(m, k); }
static void hm_put_id(nvn_hmap *m, const void *k, id o) { void *old = nvn_hm_put(m, k, (void *)CFBridgingRetain(o)); if (old) CFRelease(old); }
static void hm_del_id(nvn_hmap *m, const void *k) { void *old = nvn_hm_del(m, k); if (old) CFRelease(old); }
static nvn_hmap g_by_serial = {.klen = 8};     // Serial → MTLTexture / NvPkg / NvProg
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
// Serial table generation: +1 when entries are added / removed or a packaged texture is replaced. Downstream caches (texture views, programs) record the generation at fetch time; if it is unchanged they can be used directly without another table lookup
static _Atomic uint64_t g_ser_gen = 1;
// Textures shared by storage (non-packaged): key = memory pool + offset + texture description. An NVN texture object is only a descriptor; an object rebuilt over the same storage with the same description is the same texture;
// SMBW rebuilds its transient render targets on new objects every frame and never Finalizes them, so building one per serial leaked about 13 MB of GPU memory per frame. Textures here are kept resident by the sharing table,
// and when the serial table replaces / deletes them it only drops its reference (defer_obj_unref), never removing them from the residency set. The sharing table does not evict (entries = distinct storage + description combinations).
typedef struct { uint64_t pool; int64_t off; nvn_texdesc d; } alias_key;
static nvn_hmap g_alias = {.klen = sizeof(alias_key)};
static CFMutableSetRef g_alias_set;   // Textures in the sharing table (non-owning; used to ask "is this shared")
static uint64_t g_alias_evicted;   // shared textures evicted on pool Finalize so far (for the memory report)
static int alias_has(id o) { return g_alias_set && o && CFSetContainsValue(g_alias_set, (__bridge const void *)o); }
// On memory pool Finalize, evict the textures built on that pool from the sharing table (collect the keys first, then delete: deletion shifts entries back). The sharing table used not to evict: Mario's pools stay for good, so entries are bounded;
// Engage frees pools on scene changes and rebuilds on new ones, so the old pools' render targets and other private textures stayed in the table forever (about 0.8 GB more GPU memory over 850 s of the title demo), and linear textures built directly on a pool also kept the whole pool buffer alive.
// Also, when a pool object's address is reused, old entries would hand a texture on the old storage to the new pool's texture at the same offset and description. Evicted textures go through deferred release; if the serial table still holds one, that is just one more reference
static void treg_drop_pool_views(const void *pool);
void mtl_alias_drop_pool(const void *pool) {
    if (!pool) return;
    pthread_mutex_lock(&g_smu);
    uint32_t n = 0; alias_key *ks = NULL;
    for (uint32_t i = 0; i < g_alias.cap; i++) {
        if (!g_alias.vals[i]) continue;
        alias_key k; memcpy(&k, g_alias.keys + (size_t)i * g_alias.klen, sizeof k);
        if (k.pool != (uint64_t)(uintptr_t)pool) continue;
        alias_key *nk = realloc(ks, (n + 1) * sizeof *ks); if (!nk) break; ks = nk; ks[n++] = k;
    }
    for (uint32_t j = 0; j < n; j++) {
        void *old = nvn_hm_del(&g_alias, &ks[j]);
        if (!old) continue;
        id o = CFBridgingRelease(old);
        if (g_alias_set) CFSetRemoveValue(g_alias_set, (__bridge const void *)o);
        defer_obj_release(o);
    }
    if (n) { g_ser_gen++; g_alias_evicted += n; }
    pthread_mutex_unlock(&g_smu);
    free(ks);
    treg_drop_pool_views(pool);   // outside g_smu: the texture ID registry lock g_rmu is taken before g_smu
}
static __unsafe_unretained id serial_get(uint64_t k) { pthread_mutex_lock(&g_smu); __unsafe_unretained id o = hm_get_id(&g_by_serial, &k); pthread_mutex_unlock(&g_smu); return o; }
static void serial_put(uint64_t k, id o) { pthread_mutex_lock(&g_smu); id old = hm_get_id(&g_by_serial, &k); if (old) { if (alias_has(old)) defer_obj_unref(old); else defer_obj_release(old); } hm_put_id(&g_by_serial, &k, o); g_ser_gen++; pthread_mutex_unlock(&g_smu); }
static void serial_del(uint64_t k) { pthread_mutex_lock(&g_smu); id old = hm_get_id(&g_by_serial, &k); if (old) { if (alias_has(old)) defer_obj_unref(old); else defer_obj_release(old); hm_del_id(&g_by_serial, &k); g_ser_gen++; } pthread_mutex_unlock(&g_smu); }   // nothing to drop: the texture ID caches stay valid
// Texture serials owned by a live NVN texture object (TextureInitialize until Finalize or re-initialization, hle_nvn.c). Command records and texture pool registrations snapshot the
// texture object (tex_snap in hle_nvn.c, mtl_reg_tex), so replay can resolve a texture the game has already finalized: Breath of the Wild records each frame's transient render targets
// (the HUD captures, the 640x360 half-resolution buffers) and finalizes their objects before it submits. Those serials went back into g_by_serial and stayed there for good, as no second
// Finalize comes: about 10 a frame, 200,000 after an hour in the first shrine. A dead serial resolves through g_ghost instead, a small table whose oldest entry goes when a new one comes,
// so a record replayed again soon still finds its texture
enum { NGHOST = 512 };
static nvn_hmap g_tex_live = {.klen = 8};   // serial → (void *)1, under g_smu
static nvn_hmap g_ghost = {.klen = 8}; static uint64_t g_ghost_ring[NGHOST]; static unsigned g_ghost_next; static uint64_t g_ghost_puts;
void mtl_tex_init(const tex_t *t) { if (!t->serial) return; pthread_mutex_lock(&g_smu); nvn_hm_put(&g_tex_live, &t->serial, (void *)1); pthread_mutex_unlock(&g_smu); }
static void obj_drop(id o) { if (alias_has(o)) defer_obj_unref(o); else defer_obj_release(o); }   // caller holds g_smu
static __unsafe_unretained id tex_get(uint64_t k) {
    pthread_mutex_lock(&g_smu); __unsafe_unretained id o = hm_get_id(&g_by_serial, &k); if (!o && g_ghost.n) o = hm_get_id(&g_ghost, &k); pthread_mutex_unlock(&g_smu); return o;
}
static void tex_put(uint64_t k, id o) {   // serials from 2^62 up are external textures (mtl_ext_tex), never initialized by the game
    pthread_mutex_lock(&g_smu);
    nvn_hmap *m = k >= (1ull << 62) || nvn_hm_get(&g_tex_live, &k) ? &g_by_serial : &g_ghost;
    id old = hm_get_id(m, &k);
    if (old) obj_drop(old);
    else if (m == &g_ghost) {
        uint64_t *slot = &g_ghost_ring[g_ghost_next++ % NGHOST];
        if (*slot) { id ev = hm_get_id(&g_ghost, slot); if (ev) obj_drop(ev); hm_del_id(&g_ghost, slot); }
        *slot = k; g_ghost_puts++;
    }
    hm_put_id(m, &k, o); g_ser_gen++;
    pthread_mutex_unlock(&g_smu);
}
// Host record of a packaged texture: the current MTLTexture, the upload stamp, and the global write generation at the last check
// spare: the texture the last re-upload replaced, kept resident and written by the next re-upload once the GPU has passed spare_fence (a texture the CPU rewrites every frame, a movie frame,
// alternates between two textures instead of allocating one per frame: Link's Awakening's title movie made and freed an 8 MB texture a frame)
@interface NvPkg : NSObject { @public id<MTLTexture> tex, spare; uint64_t stamp, seen, hash, spare_fence; const void *span; } @end   // hash: full hash of the uploaded content under DM_NVN_DIRTYCHECK; span: its tracked range (nvn_dirty_span, NULL = untracked)
@implementation NvPkg
- (void)dealloc { res_remove(tex); if (spare) res_remove(spare); }
@end
static uint64_t g_pkg_uploads, g_pkg_reuploads, g_pkg_miss;
static uint64_t g_pfv_upgrades;
// DM_NVN_DIRTYCHECK=1 (diagnostic, compared against ground truth): fully hash the pool content on every use; changed but not detected by dirty-page tracking = a missed detection
static int dirtycheck(void) { static int v = -1; if (v < 0) v = getenv("DM_NVN_DIRTYCHECK") != NULL; return v; }
static uint64_t full_hash(const uint8_t *m, uint64_t n) { uint64_t h = 1469598103934665603ull; for (uint64_t i = 0; i < n; i++) h = (h ^ m[i]) * 1099511628211ull; return h; }
#define NVN_UPDIAG_LOG MLOG
#include "nvn_upload_diag.h"
#undef NVN_UPDIAG_LOG
#define NVN_CPU_CACHE_LOG MLOG
#include "nvn_cpu_decode_cache.h"
#undef NVN_CPU_CACHE_LOG
// The GPU does not support BC compressed formats (A15 / A16 etc.). BC1-3 are
// transcoded block-for-block to ASTC 4x4; the remaining BC formats are created
// in decompressed form. DM_TEST_NO_BC=1 forces this path on any machine.
static int no_bc(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_TEST_NO_BC");
#if TARGET_OS_IPHONE && __IPHONE_OS_VERSION_MIN_REQUIRED < 160400
        v = 1;
#else
        v = (e && *e && strcmp(e, "0")) || ![g_dev supportsBCTextureCompression];
#endif
        if (v) MLOG("GPU BC compression is unavailable%s: transcode BC1-3 to ASTC 4x4 and decompress the remaining BC formats",
                    e && *e && strcmp(e, "0") ? " (forced by DM_TEST_NO_BC)" : ""); }
    return v;
}
static int bc_unpack(uint32_t nvn_fmt, uint32_t *pf, int *bpp) { return no_bc() && nvn_bc_unpacked(nvn_fmt, pf, bpp); }
// BC1/2/3 use 4x4 blocks just like ASTC 4x4. The runtime transcoder replaces
// one source block with one valid ASTC block, retaining compressed residency.
// 3D and multisample textures keep the proven decompression path.
static int no_bc_astc_mode(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NO_BC_ASTC");
        v = !e || !*e || !strcmp(e, "all") ? 2 : (!strcmp(e, "bc1") ? 1 : 0);
        MLOG("No-BC storage: DM_NO_BC_ASTC=%s (%s)", e && *e ? e : "all",
             v == 2 ? "keep BC1/2/3 compressed as ASTC" : v == 1 ? "keep only BC1 compressed as ASTC" : "decompress all BC formats for comparison");
    }
    return v;
}
static int bc_astc(const tex_t *t, uint32_t *pf) {
    if (!no_bc()) return 0;
    uint32_t apf; int kind, mode = no_bc_astc_mode();
    if (!mode || !nvn_bc_astc_fmt((uint32_t)t->d.format, &apf, &kind) || (mode == 1 && kind != 1)) return 0;
    if (t->d.target == NVN_TEXTURE_TARGET_3D || t->d.samples > 1) return 0;
    *pf = apf;
    return 1;
}
static int is_astc4(MTLPixelFormat f) { return f == (MTLPixelFormat)DM_MTL_ASTC4 || f == (MTLPixelFormat)DM_MTL_ASTC4_SRGB; }
static int is_native_bc(MTLPixelFormat f) { return f >= (MTLPixelFormat)DM_MTL_BC1 && f <= (MTLPixelFormat)DM_MTL_BC7_SRGB; }
static void tex_tag_format(id<MTLTexture> mt, uint32_t nvn_fmt);
static void tex_format_stat(void);
static MTLTextureDescriptor *tex_desc(const tex_t *t, int packaged) {
    uint32_t pf = 0; uint8_t swz[4];
    if (!nvn_fmt_to_mtl(t->d.format, &pf, swz) || !pf) { RED_ONCE(0, "贴图格式 %#x Metal 无对应（首例）", t->d.format); return nil; }
    int unp = 0, astc = 0; { uint32_t upf; int ubpp; if (bc_astc(t, &upf)) { pf = upf; astc = 1; } else if (bc_unpack(t->d.format, &upf, &ubpp)) { pf = upf; unp = 1; } }
    // Metal's limits: 16384 in 2D, 2048 in 3D, 2048 array layers; beyond them Metal aborts the process on an assertion, so they are caught here with a red line
    if (t->d.width > 16384 || t->d.height > 16384 || t->d.depth > 2048 || t->d.levels > 15) { RED_ONCE(71, "贴图尺寸超出 Metal 上限：%u×%u×%u 级 %u 格式 %#x 目标 %d（首例 ser=%llu）", t->d.width, t->d.height, t->d.depth, t->d.levels, t->d.format, t->d.target, (unsigned long long)t->serial); return nil; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.pixelFormat = (MTLPixelFormat)pf;
    d.width = t->d.width ? t->d.width : 1; d.height = t->d.height ? t->d.height : 1; d.depth = 1; d.arrayLength = 1;
    d.mipmapLevelCount = t->d.levels ? t->d.levels : 1;
    switch (t->d.target) {
        // 1D / 1D array are created as one-row 2D / 2D array textures: translated shaders never declare texture1d (0 in all five games' libraries) and always sample them as 2D; a Metal 1D texture bound to a texture2d argument is a type mismatch with undefined reads.
        // Super Mario 3D World + Bowser's Fury's material shaders sample 64x1 R32F 1D lookup tables heavily every frame (on Maxwell a 2D sample of a 1D texture reads that one row)
        case 0: d.textureType = MTLTextureType2D; d.height = 1; break;
        case 2: d.textureType = MTLTextureType3D; d.depth = t->d.depth; break;
        case 3: d.textureType = MTLTextureType2DArray; d.arrayLength = t->d.height; d.height = 1; break;
        case 4: d.textureType = MTLTextureType2DArray; d.arrayLength = t->d.depth; break;
        case 5: d.textureType = MTLTextureType2DMultisample; d.sampleCount = t->d.samples; d.mipmapLevelCount = 1; break;
        case 6: d.textureType = MTLTextureType2DMultisampleArray; d.sampleCount = t->d.samples; d.arrayLength = t->d.depth; d.mipmapLevelCount = 1; break;
        // Cube textures are created as 6-layer 2D arrays and sampled through cube / cube array views made by tex_fit: Metal only allows cube array views of a 2D array base texture (not of a cube base).
        // The Legend of Zelda: Breath of the Wild has 1512 shaders that sample the environment light probes as a cube array while a single cube texture is bound (texture ID 1064 onwards): with mismatched types the reads are undefined, and every lit surface came out black
        case 8: d.textureType = MTLTextureType2DArray; d.arrayLength = 6; break;
        // NVN gives a cube array's depth in layer-faces (a multiple of 6), while
        // Metal's cube-array length counts cubes. Keep the face layers in a 2D
        // array and let tex_fit create the compatible cube-array view.
        case 9: d.textureType = MTLTextureType2DArray; d.arrayLength = t->d.depth; break;
        case 10: RED_ONCE(1, "缓冲贴图（TARGET_BUFFER）未实现"); return nil;
        default: d.textureType = MTLTextureType2D; break;
    }
    d.storageMode = MTLStorageModePrivate;   // Render targets and assets both go in GPU private storage (lossless compression available)
    d.usage = MTLTextureUsageShaderRead | (fmt_compressed(t->d.format) || packaged ? 0 : MTLTextureUsageRenderTarget)
            | (t->d.flags & NVN_TEXTURE_FLAGS_IMAGE ? MTLTextureUsageShaderWrite : 0)   // Image usage: shader-writable (compute kernels generating the 3D color lookup table, etc.)
            | (astc ? MTLTextureUsagePixelFormatView : 0)
            | (unp ? MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView : 0);   // No BC: textures in the decompressed format are written by the GPU decompression kernel (up_dec) / receive whole-block copies from ASTC sources (copy_t2t_astc)
    // iOS 16 validation requires format-view usage for the X32 stencil plane.
    // Declare it at allocation, before the combined texture becomes a live attachment.
    if (pf == MTLPixelFormatDepth32Float_Stencil8) d.usage |= MTLTextureUsagePixelFormatView;
    return d;
}
// In-place upgrade: when the original texture was not created with PixelFormatView (off by default, to keep Apple GPU lossless compression) but a different-format view is needed, create a new one with the original spec + PixelFormatView, copy over every slice × level, and replace the registration.
// Count: across the 4 screens character select / camp / wilderness / combat, 17473 textures needed a different-format view 0 times, so this is a fallback and does not rely on predicting which textures need it
static id<MTLTexture> tex_upgrade_pfv(const tex_t *t, id<MTLTexture> base, MTLPixelFormat view_pf) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = base.textureType; d.pixelFormat = base.pixelFormat; d.width = base.width; d.height = base.height; d.depth = base.depth;   // Keep the original format, only add the format-view usage
    d.arrayLength = base.arrayLength; d.mipmapLevelCount = base.mipmapLevelCount; d.sampleCount = base.sampleCount;
    d.storageMode = base.storageMode; d.cpuCacheMode = base.cpuCacheMode; d.hazardTrackingMode = base.hazardTrackingMode;
    d.resourceOptions = base.resourceOptions; d.usage = base.usage | MTLTextureUsagePixelFormatView;
    id<MTLTexture> upgraded = [g_dev newTextureWithDescriptor:d];
    if (!upgraded) { RED_ONCE(45, "贴图升级失败 ser=%llu 格式 %lu → %lu", t->serial, (unsigned long)base.pixelFormat, (unsigned long)view_pf); return base; }
    tex_tag_format(upgraded, t->d.format);
    res_add(upgraded);
    id<MTLBlitCommandEncoder> b = blit();
    NSUInteger slices = base.textureType == MTLTextureType3D ? 1 : base.arrayLength * (base.textureType == MTLTextureTypeCube || base.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    for (NSUInteger l = 0; l < base.mipmapLevelCount; l++) {
        NSUInteger w = MAX(base.width >> l, 1u), h = MAX(base.height >> l, 1u), z = base.textureType == MTLTextureType3D ? MAX(base.depth >> l, 1u) : 1;
        if (base.textureType == MTLTextureType3D)
            enc_b_copy_texture(b, base, 0, l, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, z), upgraded, 0, l, MTLOriginMake(0, 0, 0));
        else for (NSUInteger s = 0; s < slices; s++)
            enc_b_copy_texture(b, base, s, l, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, 1), upgraded, s, l, MTLOriginMake(0, 0, 0));
    }
    id holder = tex_get(t->serial);
    if ([holder isKindOfClass:[NvPkg class]]) { id<MTLTexture> old = ((NvPkg *)holder)->tex; ((NvPkg *)holder)->tex = upgraded; defer_obj_release(old); g_ser_gen++; }
    else {
        tex_put(t->serial, upgraded);
        alias_key ak; memset(&ak, 0, sizeof ak); ak.pool = (uint64_t)(uintptr_t)t->pool; ak.off = t->off; ak.d = t->d;   // Switch the sharing table to the upgraded one too, so later objects on the same storage get it directly
        pthread_mutex_lock(&g_smu); if (hm_get_id(&g_alias, &ak) == base) { hm_put_id(&g_alias, &ak, upgraded); CFSetAddValue(g_alias_set, (__bridge const void *)upgraded); } pthread_mutex_unlock(&g_smu);
    }
    MLOG("贴图升级为可换格式视图 ser=%llu 基格式 %lu → 视图格式 %lu（升级次数=%llu）", t->serial, (unsigned long)base.pixelFormat, (unsigned long)view_pf, ++g_pfv_upgrades);
    return upgraded;
}
// Packaged texture upload: first set read-only, then take the stamp, then read the pool memory and deswizzle into a staging buffer, and blit into a new private texture.
// Batching: uploads within one replay submission share one upload command buffer (compute encoder deswizzle + a unified blit), flushed before the replay command buffer is committed;
// the same queue executes in commit order ⇒ uploads are always finished before the first sample. Uploads outside a submission are flushed on the spot. The staging area is cut from 32 MB private blocks, held by the command buffer until completion.
static _Atomic uint64_t g_upload_ns;   // Cumulative time of deswizzle + upload encoding on the submit thread (timing instrumentation)
static _Atomic uint64_t g_upload_gpu_ns;
static id<MTLCommandBuffer> g_up_cb;
static id<MTLComputeCommandEncoder> g_up_ce;
static _Atomic int g_up_alloc_busy[3];
// Waiting for upload batches to finish (g_up_pending, g_up_alloc_busy): the batches' completion handler broadcasts after it updates them, so waiters sleep until then (they used to poll every 0.2–1 ms;
// Three Houses' loader calls QueueFinish after each resource, each one waited up to a millisecond late). The 10 ms timeout is only a safety net
static pthread_mutex_t g_up_wmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_up_wcv = PTHREAD_COND_INITIALIZER;
static void up_wake(void) { pthread_mutex_lock(&g_up_wmu); pthread_cond_broadcast(&g_up_wcv); pthread_mutex_unlock(&g_up_wmu); }
static void up_wait_zero(_Atomic int *v) {
    if (!atomic_load(v)) return;
    pthread_mutex_lock(&g_up_wmu);
    while (atomic_load(v)) { struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 10000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; } pthread_cond_timedwait(&g_up_wcv, &g_up_wmu, &dl); }
    pthread_mutex_unlock(&g_up_wmu);
}
static unsigned g_up_seq;
#define UP_BLOCK_SIZE (32u * 1024u * 1024u)
// Each of the three upload slots keeps one staging block of UP_SLOT_BLOCK for good (resident, reused once that slot's previous batch has finished: up_begin waits for it). Every batch used to make a fresh
// 32 MB private block, put it in the residency set and retire it after the GPU was done: Breath of the Wild re-uploads a few CPU-written textures each frame (about 40 KB each), so that was about 1.7 blocks
// a frame, about 3 GB/s of GPU memory allocated, made resident and freed, and 2.4 residency commits a frame. A slot's block grows to fit the first texture of a batch (up to UP_BLOCK_SIZE); what does
// not fit after that continues in fresh blocks of UP_BLOCK_SIZE as before
#define UP_SLOT_BLOCK (4u * 1024u * 1024u)
static __strong id<MTLBuffer> g_up_slot_blk[3]; static unsigned g_up_cur_slot;
typedef struct {
    __unsafe_unretained id<MTLBuffer> src;
    size_t off, row, image;
    MTLSize size;
    __unsafe_unretained id<MTLTexture> dst;   // Strongly referenced by g_up_texs until the flush is encoded
    NSUInteger slice, level;
    uint8_t fill;   // zero initialization; encoded before transcode/decode work
} up_blit;
static up_blit *g_up_blits; static int g_nup_blits, g_up_blitcap;
// Without BC, BC1–5 are decompressed on the GPU (A16 logs: CPU deswizzle + CPU decompression took over 300 ms in a single frame): the GPU deswizzle writes the compressed blocks into the staging area, and at flush time, after the barrier,
// the compute kernel bcd decodes them into the texture one thread per block (the target is a 2D view in the decompressed format; an sRGB base texture is written with raw values through an RGBA8Unorm view). BC6H / BC7 / 3D textures are very few and still go through the CPU (nvn_bcdec.c)
typedef struct { __unsafe_unretained id<MTLBuffer> src, params; size_t off, poff; uint32_t bw, bh; __unsafe_unretained id<MTLTexture> view; } up_dec;   // view: held by g_up_texs until this upload batch (not the whole replay submission) finishes
static up_dec *g_up_decs; static int g_nup_decs, g_up_deccap;
// BC1-3 stored as ASTC: GPU deswizzle first writes BC blocks into src, then
// bca transcodes them into a disjoint region of that same staging buffer. A
// following blit encoder copies the ASTC blocks into the compressed texture.
typedef struct { __unsafe_unretained id<MTLBuffer> src, params; size_t poff; uint32_t nbx, nby; } up_ast;
static up_ast *g_up_asts; static int g_nup_asts, g_up_astcap;
static id<MTLComputePipelineState> bca_pso(void) {
    static id<MTLComputePipelineState> ps; static int failed;
    if (!ps && !failed) {
        NSError *e = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(nvn_bca_msl) options:nil error:&e];
        id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"bca"] : nil;
        ps = fn ? nvn_cached_compute(g_dev, fn, &e) : nil;
        if (!ps) { failed = 1; MLOG("[RED] No-BC GPU BC-to-ASTC pipeline creation failed: %s; falling back to CPU transcoding", e.localizedDescription.UTF8String); }
        else MLOG("No-BC storage: transcoding BC1-3 to ASTC 4x4 on the GPU");
    }
    return ps;
}
static const char k_bcd_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct P { uint off, pitch, w, h, fmt; };\n"   // fmt: 1 BC1, 2 BC2, 3 BC3, 4 BC4, 5 BC4 signed, 6 BC5, 7 BC5 signed
"static float3 c565(uint c) { return float3((c >> 11) & 31, (c >> 5) & 63, c & 31) / float3(31, 63, 31); }\n"
"static void col(device const uchar *b, thread float4 *o, bool four) {\n"
"  uint c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8), ix = b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24); float3 e0 = c565(c0), e1 = c565(c1); float4 p[4];\n"
"  p[0] = float4(e0, 1); p[1] = float4(e1, 1);\n"
"  if (four || c0 > c1) { p[2] = float4((2 * e0 + e1) / 3, 1); p[3] = float4((e0 + 2 * e1) / 3, 1); } else { p[2] = float4((e0 + e1) / 2, 1); p[3] = float4(0); }\n"
"  for (int i = 0; i < 16; i++) o[i] = p[(ix >> (2 * i)) & 3]; }\n"
"static void a8(device const uchar *b, thread float *o, bool sn) {\n"   // Single-channel block of BC4 / BC3 alpha / BC5
"  float a0 = sn ? max(float(as_type<char>(b[0])), -127.0) / 127 : b[0] / 255.0, a1 = sn ? max(float(as_type<char>(b[1])), -127.0) / 127 : b[1] / 255.0;\n"
"  bool eight = sn ? as_type<char>(b[0]) > as_type<char>(b[1]) : b[0] > b[1]; float lo = sn ? -1 : 0; float p[8]; p[0] = a0; p[1] = a1;\n"
"  for (int k = 2; k < 8; k++) p[k] = eight ? ((8 - k) * a0 + (k - 1) * a1) / 7 : (k < 6 ? ((6 - k) * a0 + (k - 1) * a1) / 5 : (k == 6 ? lo : 1));\n"
"  uint lo24 = b[2] | (b[3] << 8) | (b[4] << 16), hi24 = b[5] | (b[6] << 8) | (b[7] << 16);\n"
"  for (int i = 0; i < 16; i++) o[i] = p[i < 8 ? (lo24 >> (3 * i)) & 7 : (hi24 >> (3 * (i - 8))) & 7]; }\n"
"kernel void bcd(device const uchar *src [[buffer(0)]], device const P &p [[buffer(1)]], texture2d<float, access::write> dst [[texture(0)]], uint2 g [[thread_position_in_grid]]) {\n"
"  uint nbx = (p.w + 3) / 4, nby = (p.h + 3) / 4; if (g.x >= nbx || g.y >= nby) return;\n"
"  uint bb = (p.fmt == 1 || p.fmt == 4 || p.fmt == 5) ? 8 : 16; device const uchar *b = src + p.off + g.y * p.pitch + g.x * bb; float4 o[16]; float a[16], c[16];\n"
"  if (p.fmt == 1) col(b, o, false);\n"
"  else if (p.fmt == 2) { col(b + 8, o, true); for (int i = 0; i < 16; i++) o[i].a = ((b[i / 2] >> (4 * (i & 1))) & 15) / 15.0; }\n"
"  else if (p.fmt == 3) { col(b + 8, o, true); a8(b, a, false); for (int i = 0; i < 16; i++) o[i].a = a[i]; }\n"
"  else if (p.fmt == 4 || p.fmt == 5) { a8(b, a, p.fmt == 5); for (int i = 0; i < 16; i++) o[i] = float4(a[i], 0, 0, 1); }\n"
"  else { a8(b, a, p.fmt == 7); a8(b + 8, c, p.fmt == 7); for (int i = 0; i < 16; i++) o[i] = float4(a[i], c[i], 0, 1); }\n"
"  for (int i = 0; i < 16; i++) { uint2 q = g * 4 + uint2(i & 3, i >> 2); if (q.x < p.w && q.y < p.h) dst.write(o[i], q); } }\n";
static int bcd_fmt(uint32_t f) {   // NVN format → bcd fmt; 0 = not decoded on the GPU (BC6H / BC7 / non-BC)
    switch (f) { case 0x41: case 0x42: case 0x45: case 0x46: return 1; case 0x43: case 0x47: return 2; case 0x44: case 0x48: return 3;
                 case 0x49: return 4; case 0x4a: return 5; case 0x4b: return 6; case 0x4c: return 7; default: return 0; }
}
static id<MTLComputePipelineState> bcd_pso(void) {
    static id<MTLComputePipelineState> ps; static int failed;
    if (!ps && !failed) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_bcd_msl) options:nil error:&e]; id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"bcd"] : nil;
        ps = fn ? nvn_cached_compute(g_dev, fn, &e) : nil;
        if (!ps) { failed = 1; MLOG("【红】无 BC：GPU 解压管线建不出 %s（退回 CPU 解压）", e.localizedDescription.UTF8String); } else MLOG("无 BC：BC1–5 改在 GPU 上解压"); }
    return ps;
}
// Every target and temporary writable view referenced by the batch.  A
// scene-load replay can create more than 1,500 BC-decompression views before
// its terminal fence.  Deferring those views to that fence pins their parent
// textures while the CPU is still translating the replay and can cross tvOS's
// memory limit.  The upload command buffer is their exact GPU owner, so this
// array is transferred to its completion handler at flush time.
static NSMutableArray *g_up_texs;
static id<MTLBuffer> g_up_block;
static size_t g_up_block_off, g_up_block_size;
static NSMutableArray *g_up_keep;
// Limit overflow staging per batch. The three upload slots bound in-flight
// batches. A single texture larger than the budget gets its own block (uploads
// are not tiled here).
static size_t g_up_staging_bytes;
#define UP_BATCH_BYTES (128u * 1024u * 1024u)
// BC1-5 expands to ordinary RGBA/RG textures on Apple GPUs without native BC.
// The compressed staging footprint can therefore stay below UP_BATCH_BYTES
// while one upload command buffer accumulates hundreds of MiB of writable
// target textures.  Bound that distinct lifetime as well.  Three upload slots
// mean a fourth batch waits for the first one's completion, limiting ordinary
// in-flight decompressed targets to about 3 * this value.
#ifndef UP_BATCH_TARGET_BYTES
#define UP_BATCH_TARGET_BYTES (64u * 1024u * 1024u)
#endif
static size_t g_up_target_bytes, g_up_target_peak;
static uint64_t g_up_target_splits;


// The upload command buffer was discarded by the system (dropped along during GPU error recovery; the first frame on entering Travincal drops one every run): the objects of this batch of textures are built and the timestamps updated, but the content was never written in,
// and if the pool memory on the game side no longer changes it will never be re-uploaded. Record this batch of target textures, and the next time any of them is fetched treat it as dirty and re-upload once.
// DM_TEST_UP_LOST=<batch index> (for testing): treat that batch of uploads as dropped, to verify the re-upload path.
static NSHashTable *g_up_lost; static os_unfair_lock g_up_lost_lk = OS_UNFAIR_LOCK_INIT; static _Atomic int g_up_lost_n; static _Atomic uint64_t g_up_lost_redo;   // Weak-reference table: if a texture in the record is never fetched again and is released normally, this table does not keep it alive
static int up_lost_take(id tex) {   // This texture's upload was dropped: take it out of the record and return 1
    if (!tex) return 0; os_unfair_lock_lock(&g_up_lost_lk); int hit = [g_up_lost containsObject:tex]; if (hit) { [g_up_lost removeObject:tex]; atomic_store(&g_up_lost_n, (int)g_up_lost.allObjects.count); atomic_fetch_add(&g_up_lost_redo, 1); } os_unfair_lock_unlock(&g_up_lost_lk); return hit;
}
static void up_diag(void);
static void up_flush(void) {
    up_owner_check("up_flush");
    if (!g_up_cb) return;
    if (g_up_ce) {
        m4_end(g_up_ce); g_up_ce = nil;
    }
    // Three encoder phases give explicit ordering on traditional Metal:
    // transparent zero fills -> BC decode/transcode -> uploaded content.
    // This prevents a large ASTC copy from overtaking its transcoder and
    // decoding as Metal's magenta invalid-block colour.
    int nfill = 0; for (int i = 0; i < g_nup_blits; i++) nfill += g_up_blits[i].fill;
    id<MTLBlitCommandEncoder> b = nfill ? [g_up_cb blitCommandEncoder] : nil;
    for (int i = 0; b && i < g_nup_blits; i++) {
        up_blit *x = &g_up_blits[i]; if (!x->fill) continue;
        enc_b_copy_buffer_texture(b, x->src, x->off, x->row, x->image, x->size, x->dst, x->slice, x->level, MTLOriginMake(0, 0, 0));
    }
    [b endEncoding];
    id<MTLComputeCommandEncoder> dc = g_nup_decs || g_nup_asts ? [g_up_cb computeCommandEncoder] : nil;
    if (dc) {
        if (g_nup_decs) {
            [dc setComputePipelineState:bcd_pso()];
            for (int i = 0; i < g_nup_decs; i++) {
                up_dec *x = &g_up_decs[i];
                [dc setBuffer:x->src offset:0 atIndex:0];
                [dc setBuffer:x->params offset:x->poff atIndex:1];
                [dc setTexture:x->view atIndex:0];
                [dc dispatchThreadgroups:MTLSizeMake((x->bw + 7) / 8, (x->bh + 7) / 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            }
        }
        if (g_nup_asts) {
            [dc setComputePipelineState:bca_pso()];
            for (int i = 0; i < g_nup_asts; i++) {
                up_ast *x = &g_up_asts[i];
                [dc setBuffer:x->src offset:0 atIndex:0];
                [dc setBuffer:x->params offset:x->poff atIndex:1];
                [dc setBuffer:x->src offset:0 atIndex:2];
                [dc dispatchThreadgroups:MTLSizeMake((x->nbx + 7) / 8, (x->nby + 7) / 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            }
        }
    }
    [dc endEncoding]; g_up_ce = nil;
    b = g_nup_blits > nfill ? [g_up_cb blitCommandEncoder] : nil;
    for (int i = 0; b && i < g_nup_blits; i++) {
        up_blit *x = &g_up_blits[i]; if (x->fill) continue;
        enc_b_copy_buffer_texture(b, x->src, x->off, x->row, x->image, x->size, x->dst, x->slice, x->level, MTLOriginMake(0, 0, 0));
    }
    [b endEncoding];
    id<MTLCommandBuffer> cb = g_up_cb;
    unsigned slot = (g_up_seq - 1) % 3;
    // Only this upload batch uses its overflow and CPU staging. Retire it as
    // soon as the upload completes, even while the main rendering submission
    // is still being built.
    NSMutableArray *staging = g_up_keep;
    NSMutableArray *batch_refs = g_up_texs;
    if (g_up_block && g_up_block != g_up_slot_blk[slot]) [staging addObject:g_up_block];
    g_up_keep = nil; g_up_texs = nil; g_up_block = nil;
    g_up_block_off = g_up_block_size = g_up_staging_bytes = g_up_target_bytes = 0;
    NSMutableArray *dsts = [NSMutableArray arrayWithCapacity:(NSUInteger)g_nup_blits]; for (int i = 0; i < g_nup_blits; i++) if (g_up_blits[i].dst) [dsts addObject:g_up_blits[i].dst];
    static int lost_at = -2; static uint64_t batch_n; if (lost_at == -2) { const char *e = getenv("DM_TEST_UP_LOST"); lost_at = e ? atoi(e) : -1; } int fake_lost = dsts.count && lost_at > 0 && (int)++batch_n == lost_at;

    atomic_fetch_add(&g_up_pending, 1);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> f) {
        if (f.error) MLOG("【红】上传命令缓冲执行出错：%s", f.error.description.UTF8String);
        if (f.error || fake_lost) {
            os_unfair_lock_lock(&g_up_lost_lk); if (!g_up_lost) g_up_lost = [NSHashTable weakObjectsHashTable]; for (id d in dsts) [g_up_lost addObject:d]; int n = (int)g_up_lost.allObjects.count; atomic_store(&g_up_lost_n, n); os_unfair_lock_unlock(&g_up_lost_lk);
            pthread_mutex_lock(&g_smu); g_ser_gen++; pthread_mutex_unlock(&g_smu);   // Make the view cache of every texture ID take the slow path once (on a cache hit tex_mtl is not passed through and this record is not seen)
            MLOG("%s上传命令缓冲被丢：这一批 %lu 张贴图记为内容未写，下次取用时重传（待重传共 %d 张）", fake_lost ? "DM_TEST_UP_LOST：" : "", (unsigned long)dsts.count, n);
        }
        double d = f.GPUEndTime - f.GPUStartTime;
        if (d > 0) atomic_fetch_add(&g_upload_gpu_ns, (uint64_t)(d * 1e9));
        @autoreleasepool {
            for (id<MTLResource> obj in staging) res_remove(obj);
            [staging removeAllObjects];
            // Views were never separate residency allocations; releasing the
            // upload owner's references is sufficient and may also release a
            // replaced parent texture immediately.
            [batch_refs removeAllObjects];
            res_commit();
        }
        atomic_store(&g_up_alloc_busy[slot], 0);
        atomic_fetch_sub(&g_up_pending, 1);
        up_wake();
    }];
    res_commit();
    [cb commit];
    g_up_cb = nil;
    up_diag();
    g_nup_blits = 0; g_nup_decs = 0; g_nup_asts = 0;
}

static void up_begin(void) {
    up_owner_check("up_begin");
    if (g_up_cb) return;
    unsigned slot = g_up_seq++ % 3;
    up_wait_zero(&g_up_alloc_busy[slot]);
    atomic_store(&g_up_alloc_busy[slot], 1); g_up_cur_slot = slot;
    g_up_cb = [g_q commandBuffer];
    g_up_ce = [g_up_cb computeCommandEncoder];


    if (!g_up_texs) g_up_texs = [NSMutableArray array];
    if (!g_up_keep) g_up_keep = [NSMutableArray array];
}

// Start (or keep) the batch that will own one GPU-BC texture.  A texture larger
// than the budget is allowed to occupy a batch by itself; the next texture
// splits.  Charge only after successful encoding: up_alloc can independently
// flush for its staging budget and must then leave the charge on the new batch.
static void up_target_prepare(size_t bytes) {
    size_t used = MIN(g_up_target_bytes, (size_t)UP_BATCH_TARGET_BYTES);
    if (g_up_cb && g_up_target_bytes && bytes > (size_t)UP_BATCH_TARGET_BYTES - used) {
        g_up_target_splits++;
        up_flush();
    }
    up_begin();
}
static void up_target_charge(size_t bytes) {
    if (bytes > SIZE_MAX - g_up_target_bytes) MDIE("GPU-BC target byte counter overflow");
    g_up_target_bytes += bytes;
    if (g_up_target_bytes > g_up_target_peak) g_up_target_peak = g_up_target_bytes;
}

// Diagnostic readout (low frequency; printed only when the block switch count changed and ≥ 1 second since the last line, at most 200 lines per game): how many times an upload batch ran out of staging block and switched blocks midway; GPU allocated, residency set object count, deferred-release queue length.
// If the old block is not retired on a switch it stays in the residency set and can never be freed, and "GPU allocated" climbs one block at a time with the switch count
static uint64_t g_up_blocks_new, g_up_swaps, g_up_swap_bytes; static NSHashTable *g_up_old;   // g_up_old: old blocks switched out (weak references); the diagnostic line counts how many are still alive
static void up_diag(void) { @autoreleasepool {
    // Weak-table enumeration autoreleases the buffers it finds. Bound those
    // temporary references so completed batches can disappear immediately.
    // Print a line when the block switch count changes; then one more line 5 seconds later (that batch's commit is long done and deferred retirement drained, so "GPU allocated" is the settled number)
    static uint64_t said_swaps = ~0ull, said_target_splits = ~0ull, said_gpu_bucket = ~0ull, last_ns; static int lines, follow; uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    uint64_t gpu_bucket = g_dev.currentAllocatedSize >> 27;   // force a census at every additional 128 MiB, even when staging counters stay flat
    int chg = g_up_swaps != said_swaps || g_up_target_splits != said_target_splits || gpu_bucket != said_gpu_bucket, fol = follow && now - last_ns >= 5000000000ull; if ((!chg && !fol) || lines >= 200) return;
    if (chg && last_ns && now - last_ns < 1000000000ull) return; last_ns = now; said_swaps = g_up_swaps; said_target_splits = g_up_target_splits; said_gpu_bucket = gpu_bucket; lines++; follow = chg;
    unsigned long alive = 0, inres = 0; uint64_t abytes = 0; NSArray *olds = g_up_old.allObjects;
    pthread_mutex_lock(&g_res_mu); unsigned long nres = (unsigned long)g_res_objects.allObjects.count; for (id<MTLBuffer> o in olds) { alive++; abytes += o.length; inres += [g_res_objects containsObject:o]; } pthread_mutex_unlock(&g_res_mu);
    pthread_mutex_lock(&g_deferred_mu); unsigned long ndef = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    MLOG("upload staging%s: %llu blocks created, %llu mid-batch switches (%llu MB replaced); %llu GPU-BC target-budget splits, %llu MB peak target batch; %lu replaced blocks still alive (%llu MB), %lu still resident; GPU allocated %llu MB, %lu resident objects, %lu deferred releases", fol && !chg ? " (settled)" : "",
         (unsigned long long)g_up_blocks_new, (unsigned long long)g_up_swaps, (unsigned long long)(g_up_swap_bytes >> 20),
         (unsigned long long)g_up_target_splits, (unsigned long long)(g_up_target_peak >> 20), alive,
         (unsigned long long)(abytes >> 20), inres, (unsigned long long)(g_dev.currentAllocatedSize >> 20), nres, ndef);
    defer_diag_snapshot(fol && !chg ? "upload settled" : "upload batch");
    tex_format_stat();
} }
static id<MTLBuffer> up_alloc(size_t n, size_t *off) {
    size_t need = (n + 255u) & ~((size_t)255u);
    size_t extra = MAX(need, UP_BLOCK_SIZE);
    // This call starts one texture upload. Everything previously recorded can
    // be split without ending the main render pass.
    if (g_up_staging_bytes && (!g_up_block || need > g_up_block_size - g_up_block_off)
        && extra > UP_BATCH_BYTES - MIN(g_up_staging_bytes, UP_BATCH_BYTES)) {
        up_flush();
        up_begin();
    }
    if (!g_up_block && need <= UP_BLOCK_SIZE) {   // first allocation of the batch: the slot's own block, grown to the next power of two that holds it (a movie frame re-uploaded every frame, 8 MB in Link's Awakening)
        __strong id<MTLBuffer> *sb = &g_up_slot_blk[g_up_cur_slot];
        size_t want = UP_SLOT_BLOCK; while (want < need) want <<= 1;
        if (*sb && (*sb).length < want) { defer_obj_release(*sb); *sb = nil; }   // the slot's previous batch has finished (up_begin waited for it)
        if (!*sb) { *sb = [g_dev newBufferWithLength:want options:MTLResourceStorageModePrivate]; if (!*sb) MDIE("建预排布上传暂存块失败 %#zx", want); res_add(*sb); g_up_blocks_new++; }
        g_up_block = *sb; g_up_block_size = (*sb).length; g_up_block_off = 0;
        g_up_staging_bytes += (*sb).length;
    }
    if (!g_up_block || g_up_block_off + need > g_up_block_size) {
        // Block switch: the old block is still referenced by copy / decompress commands already recorded in this batch and cannot be freed on the spot; hand it to g_up_keep, to be retired later with the other staging after the flush commits (removed from the residency set on retire).
        // Previously g_up_block was simply replaced with the new block here: nobody retired the old block, yet it stayed in the residency set, so each extra block switch in an upload batch leaked one block (usually 32 MB) of GPU memory.
        // The old block and the "current block" retired at flush are two objects, so nothing is retired twice
        g_up_blocks_new++; if (g_up_block) { g_up_swaps++; g_up_swap_bytes += g_up_block_size; @autoreleasepool { if (!g_up_old) g_up_old = [NSHashTable weakObjectsHashTable]; [g_up_old addObject:g_up_block]; } static int leak = -1; if (leak < 0) leak = getenv("DM_TEST_UP_LEAK") != NULL;   // test switch DM_TEST_UP_LEAK=1: do not retire the old block, as before the fix (for comparison, to measure the leak)
            if (!leak && g_up_block != g_up_slot_blk[g_up_cur_slot]) { if (!g_up_keep) g_up_keep = [NSMutableArray array]; [g_up_keep addObject:g_up_block]; } }   // the slot's block is not retired: it stays for the slot's later batches
        g_up_block_size = n > UP_BLOCK_SIZE ? need : UP_BLOCK_SIZE;
        g_up_block = [g_dev newBufferWithLength:g_up_block_size options:MTLResourceStorageModePrivate];
        if (!g_up_block) MDIE("建预排布上传暂存块失败 %#zx", g_up_block_size);
        g_up_staging_bytes += g_up_block_size;
        res_add(g_up_block);
        g_up_block_off = 0;
    }
    *off = g_up_block_off;
    g_up_block_off += need;
    return g_up_block;
}

// Deswizzle parameter blocks: 64 bytes per cell, cut sequentially from a 64 KB shared-storage block; a full block is replaced by a new one, and the old block is released lazily by fence
static id<MTLBuffer> g_up_pblock; static size_t g_up_pblock_off;
static id<MTLBuffer> up_param_alloc(size_t *off) {
    if (!g_up_pblock || g_up_pblock_off + 64 > g_up_pblock.length) {
        if (g_up_pblock) defer_obj_release(g_up_pblock);
        g_up_pblock = [g_dev newBufferWithLength:64 << 10 options:MTLResourceStorageModeShared];
        if (!g_up_pblock) MDIE("建解排布参数块失败");
        res_add(g_up_pblock); g_up_pblock_off = 0;
    }
    *off = g_up_pblock_off; g_up_pblock_off += 64;
    return g_up_pblock;
}
static void up_blit_add(id<MTLBuffer> src, size_t off, size_t row, size_t image, MTLSize size,
                        id<MTLTexture> dst, NSUInteger slice, NSUInteger level) {
    if (g_nup_blits == g_up_blitcap) {
        g_up_blitcap = g_up_blitcap ? g_up_blitcap * 2 : 256;
        g_up_blits = realloc(g_up_blits, g_up_blitcap * sizeof *g_up_blits);
    }
    g_up_blits[g_nup_blits++] = (up_blit){src, off, row, image, size, dst, slice, level, 0};
}
static void up_blit_add_fill(id<MTLBuffer> src, size_t off, size_t row, size_t image, MTLSize size,
                             id<MTLTexture> dst, NSUInteger slice, NSUInteger level) {
    up_blit_add(src, off, row, image, size, dst, slice, level);
    g_up_blits[g_nup_blits - 1].fill = 1;
}

// A CPU-visible linear texture can normally be exposed directly as a texture view of its
// memory-pool buffer. Metal requires both the buffer offset and row stride to meet the device's
// linear-texture alignment, however. NVN permits smaller strides (Z-A uses one with a 32-byte
// row), so copy those exceptional textures into a private texture and refresh it through the
// existing dirty-page path instead of silently leaving the private fallback black.
static int linear_needs_upload(const tex_t *t, int *out_bpb, size_t *out_row) {
    if (!(t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) || !t->mem || !t->pool ||
        (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) || t->d.target != NVN_TEXTURE_TARGET_2D || t->d.levels > 1 || t->d.samples > 1) return 0;
    uint32_t pf = 0; uint8_t swz[4]; int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!nvn_fmt_to_mtl(t->d.format, &pf, swz) || !pf || !bpb || bw != 1 || bh != 1) return 0;
    size_t row = t->d.stride ? t->d.stride : (size_t)t->d.width * (size_t)bpb;
    if (out_bpb) *out_bpb = bpb; if (out_row) *out_row = row;
    void *mb = NULL; uint64_t mo = 0;
    if (!mtl_addr((uint64_t)t->mem, (uint64_t)row * (t->d.height ? t->d.height : 1), &mb, &mo)) return 1;
    NSUInteger al = [g_dev minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)pf];
    int need = mo % al || row % al;
    return need;
}

static id<MTLTexture> pkg_upload_(const tex_t *t, uint64_t *stamp, id<MTLTexture> reuse, nvn_updiag_record *diag, int reupload);
static id<MTLTexture> pkg_upload(const tex_t *t, uint64_t *stamp, id<MTLTexture> reuse, int reupload) {   // reuse: a texture of the same description, idle on the GPU, to upload into instead of a new one
    nvn_updiag_meta meta = {atomic_load(&g_cur_frame), t->serial, t->off, t->L.size,
        t->pool ? t->pool->size : 0, t->mem, t->pool,
        t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, t->d.flags, reupload};
    nvn_updiag_record diag; nvn_updiag_begin(&diag, &meta);
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    id<MTLTexture> r = pkg_upload_(t, stamp, reuse, &diag, reupload);
    clock_gettime(CLOCK_MONOTONIC, &b); uint64_t d_ = (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + b.tv_nsec - a.tv_nsec);
    atomic_fetch_add(&g_upload_ns, d_); g_up_total_ns += d_;
    nvn_updiag_end(&diag, d_, r != nil);
    return r;
}
uint64_t mtl_upload_ns_take(void) { return atomic_exchange(&g_upload_ns, 0); }
uint64_t mtl_upload_gpu_ns_take(void) { return atomic_exchange(&g_upload_gpu_ns, 0); }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = g_bp_lib_n; *pso = g_bp_pso_n; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = g_bp_lib_ns; *pso_ns = g_bp_pso_ns; }   // Cumulative time spent synchronously creating shader libraries / pipelines on the spot (the long-frame log takes the per-frame difference)
uint64_t mtl_upload_bytes(void) { return g_bp_up_bytes; }   // Cumulative texture upload bytes
static id<MTLTexture> pkg_upload_(const tex_t *t, uint64_t *stamp, id<MTLTexture> reuse, nvn_updiag_record *diag, int reupload) {
    MTLTextureDescriptor *d = tex_desc(t, 1);
    if (!d) return nil;
    id<MTLTexture> mt = reuse && reuse.pixelFormat == d.pixelFormat && reuse.width == d.width && reuse.height == d.height && reuse.mipmapLevelCount == d.mipmapLevelCount && reuse.textureType == d.textureType
                        ? reuse : [g_dev newTextureWithDescriptor:d];
    if (!mt) { RED_ONCE(2, "建 MTLTexture 失败 %ux%u 格式 %#x 目标 %d", t->d.width, t->d.height, t->d.format, t->d.target); return nil; }
    tex_tag_format(mt, t->d.format);
    if (g_allocprof > 0) { static int n; if (n++ < 30 || n % 500 == 0) MLOG("新建贴图（%s）#%d ser=%llu %ux%ux%u 格式 %#x 目标 %d 级 %u 存储 %p", __func__, n, (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, (void *)t->mem); }
    res_add(mt);   // Both first upload and re-upload register residency here (Metal 4 does not make resources resident automatically; a missed registration = GPU address page fault)
    nvn_dirty_protect(t->mem, t->L.size);
    *stamp = nvn_dirty_now();
    nvn_updiag_capture(diag);
    int linear_bpb = 0; size_t linear_row = 0;
    if (linear_needs_upload(t, &linear_bpb, &linear_row)) {
        diag->path = "cpu-linear";
        size_t w = t->d.width ? t->d.width : 1, h = t->d.height ? t->d.height : 1, tight = w * (size_t)linear_bpb;
        if (!linear_bpb || linear_row < tight || h > 1 && linear_row > (SIZE_MAX - tight) / (h - 1) || linear_row * (h - 1) + tight > t->L.size) {
            RED_ONCE(66, "线性贴图上传范围无效：%zux%zu 行距 %zu 存储 %llu", w, h, linear_row, (unsigned long long)t->L.size);
            return mt;
        }
        up_begin();
        id<MTLBuffer> stg = [g_dev newBufferWithLength:tight * h options:MTLResourceStorageModeShared];
        if (!stg) return mt;
        res_add(stg); [g_up_keep addObject:stg];
        for (size_t y = 0; y < h; y++) memcpy((uint8_t *)stg.contents + y * tight, t->mem + y * linear_row, tight);
        up_blit_add(stg, 0, tight, tight * h, MTLSizeMake(w, h, 1), mt, 0, 0);
        g_bp_up_bytes += tight * h;
        [g_up_texs addObject:mt];
        if (!g_cb) up_flush();
        static int once; if (!once++) MLOG("未对齐线性贴图：通过暂存上传并跟踪 CPU 写入");
        return mt;
    }
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    unsigned layers = (t->d.target == 4 || t->d.target == 9) ? t->d.depth : t->d.target == 8 ? 6 : t->d.target == 3 ? t->d.height : 1;   // a cube array's depth counts layer-faces (tex_desc)
    uint32_t upf; int ubpp = 0, unp = 0, astc = bc_astc(t, &upf);
    if (!astc) unp = bc_unpack(t->d.format, &upf, &ubpp);
    int akind = 0; if (astc) { uint32_t apf; nvn_bc_astc_fmt((uint32_t)t->d.format, &apf, &akind); }
    size_t total = 0;
    for (unsigned l = 0; l < d.mipmapLevelCount; l++) {
        uint32_t w = MAX(t->d.width >> l, 1u), h = MAX(t->d.height >> l, 1u), z = t->d.target == 2 ? MAX(t->d.depth >> l, 1u) : 1;
        total += astc ? (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16 * layers
                      : unp ? (size_t)w * ubpp * h * z * layers
                            : (size_t)((w + bw - 1) / bw) * bpb * ((h + bh - 1) / bh) * z * layers;
    }
    g_bp_up_bytes += total;
    void *mb; uint64_t mo;
    int gdec = unp && t->d.target != 2 && bcd_fmt(t->d.format) && bcd_pso();   // BC1–5 without BC: GPU deswizzle + GPU decompression
    int gast = astc && bca_pso();
    size_t ctotal = 0, abase = 0, atotal = 0;
    if (gast) for (unsigned l = 0; l < d.mipmapLevelCount; l++) {
        uint32_t w = MAX(t->d.width >> l, 1u), h = MAX(t->d.height >> l, 1u);
        ctotal += (size_t)((w + bw - 1) / bw) * bpb * ((h + bh - 1) / bh) * layers;
        atotal += (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16 * layers;
    }
    if (gast) abase = (ctotal + 255u) & ~(size_t)255u;
    if (gdec) for (unsigned l = 0; l < d.mipmapLevelCount; l++) { uint32_t w = MAX(t->d.width >> l, 1u), h = MAX(t->d.height >> l, 1u); ctotal += (size_t)((w + bw - 1) / bw) * bpb * ((h + bh - 1) / bh) * layers; }
    int use_gpu = (astc ? gast : (!unp || gdec)) && g_nvn_deswz_gpu && mtl_addr((uint64_t)t->mem, t->L.size, &mb, &mo);
    if (g_nvn_deswz_gpu && !use_gpu && !unp && !astc) RED_ONCE(40, "pre-tiled texture storage is outside a memory pool; falling back to CPU upload");
    if (use_gpu) {
        diag->path = gast ? "gpu-bc-astc" : gdec ? "gpu-bc" : "gpu-deswizzle";
        // The GPU reads the memory pool at execution time, which is closer to a real Switch than a snapshot at CPU upload time; the dirty-page stamp is still taken before encoding, and later writes still trigger a re-upload.
        if (gdec || gast) up_target_prepare(total); else up_begin();
        size_t stg_off;
        id<MTLBuffer> stg = up_alloc(gast ? abase + atotal : gdec ? ctotal : total ? total : 16, &stg_off);
        size_t off = 0, aoff = 0; int enc_ok = g_up_ce != nil, nb0 = g_nup_blits, nd0 = g_nup_decs, na0 = g_nup_asts;
        for (unsigned l = 0; enc_ok && l < d.mipmapLevelCount; l++) {
            uint32_t w = MAX(t->d.width >> l, 1u), h = MAX(t->d.height >> l, 1u), z = t->d.target == 2 ? MAX(t->d.depth >> l, 1u) : 1;
            size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
            for (unsigned y = 0; y < layers; y++) {
                if (nvn_deswz_gpu_encode(g_up_ce, (__bridge id<MTLBuffer>)mb, mo, &t->d, l, y, stg, stg_off + off, pitch)) { enc_ok = 0; break; }
                if (gast) {
                    uint32_t nbx = (w + 3) / 4, nby = (h + 3) / 4;
                    size_t qoff; id<MTLBuffer> q = up_param_alloc(&qoff); if (!q) { enc_ok = 0; break; }
                    uint32_t *pp = (uint32_t *)((uint8_t *)q.contents + qoff);
                    pp[0] = (uint32_t)(stg_off + off); pp[1] = (uint32_t)pitch;
                    pp[2] = (uint32_t)(stg_off + abase + aoff); pp[3] = nbx * 16;
                    pp[4] = nbx; pp[5] = nby; pp[6] = (uint32_t)akind;
                    if (g_nup_asts == g_up_astcap) { g_up_astcap = g_up_astcap ? g_up_astcap * 2 : 256; g_up_asts = realloc(g_up_asts, g_up_astcap * sizeof *g_up_asts); }
                    g_up_asts[g_nup_asts++] = (up_ast){stg, q, qoff, nbx, nby};
                    up_blit_add(stg, stg_off + abase + aoff, (size_t)nbx * 16, (size_t)nbx * 16 * nby, MTLSizeMake(w, h, 1), mt, y, l);
                    aoff += (size_t)nbx * nby * 16;
                } else if (gdec) {   // Decompression job: dispatched at flush time after the barrier (see up_flush)
                    MTLPixelFormat vf = mt.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB ? MTLPixelFormatRGBA8Unorm : mt.pixelFormat;
                    id<MTLTexture> v = [mt newTextureViewWithPixelFormat:vf textureType:MTLTextureType2D levels:NSMakeRange(l, 1) slices:NSMakeRange(y, 1)];
                    size_t qoff; id<MTLBuffer> q = up_param_alloc(&qoff);
                    if (!v || !q) { enc_ok = 0; break; }
                    uint32_t *pp = (uint32_t *)((uint8_t *)q.contents + qoff); pp[0] = (uint32_t)(stg_off + off); pp[1] = (uint32_t)pitch; pp[2] = w; pp[3] = h; pp[4] = (uint32_t)bcd_fmt(t->d.format);
                    if (g_nup_decs == g_up_deccap) { g_up_deccap = g_up_deccap ? g_up_deccap * 2 : 256; g_up_decs = realloc(g_up_decs, g_up_deccap * sizeof *g_up_decs); }
                    g_up_decs[g_nup_decs++] = (up_dec){stg, q, 0, qoff, (w + 3) / 4, (h + 3) / 4, v}; [g_up_texs addObject:v];   // Completion owns the view only until this upload CB finishes; the base texture, not this view, is resident.
                } else
                up_blit_add(stg, stg_off + off, pitch, pitch * rows, MTLSizeMake(w, h, z), mt, y, l);
                off += pitch * rows * z;
            }
        }
        if (enc_ok) {
            if (gdec || gast) up_target_charge(total);
            [g_up_texs addObject:mt];
            if (!g_cb) up_flush();
            return mt;
        }
        g_nup_blits = nb0; g_nup_decs = nd0; g_nup_asts = na0;   // Roll back this texture's pending work; an already-encoded staging deswizzle is harmless
        if (!g_cb) up_flush();
        RED_ONCE(41, "预排布贴图 GPU 解排布不支持格式 %#x 目标 %d，回退 CPU", t->d.format, t->d.target);
    }

    diag->path = astc ? "cpu-bc-astc" : unp ? "cpu-bc" : "cpu-deswizzle";
    // Cache decoded pixels, not the live texture: even an IMAGE-capable source
    // still receives its ordinary upload, including a lost-upload retry.
    nvn_cpu_decode_ticket cached = {0};
    if (unp && reupload) {
        nvn_texlayout footprint; nvn_tex_layout(&t->d, &footprint);
        // The copied source must cover every byte the descriptor's deswizzle
        // can read; inconsistent packaged layout metadata takes the old path.
        if (footprint.size && footprint.size <= t->L.size) {
            nvn_cpu_decode_key key = {t->serial, t->off, t->L.size, total, t->mem, t->pool, t->d};
            cached = nvn_cpu_decode_begin(atomic_load(&g_cur_frame), &key);
        }
        if (cached.hit) diag->path = "cpu-bc-cached";
    }
    up_begin();
    id<MTLBuffer> stg = [g_dev newBufferWithLength:total ? total : 16 options:MTLResourceStorageModeShared];
    res_add(stg); [g_up_keep addObject:stg];
    const uint8_t *decode_source = cached.entry ? cached.source : t->mem;
    uint8_t *decode_target = cached.entry ? cached.decoded : stg.contents;
    int decoded_ok = 1;
    size_t off = 0;
    for (unsigned l = 0; l < d.mipmapLevelCount; l++) {
        uint32_t w = MAX(t->d.width >> l, 1u), h = MAX(t->d.height >> l, 1u), z = t->d.target == 2 ? MAX(t->d.depth >> l, 1u) : 1;
        size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
        for (unsigned y = 0; y < layers; y++) {
            if (astc) {
                uint8_t *cb = malloc(pitch * rows); size_t ap = (size_t)((w + 3) / 4) * 16;
                if (!cb || nvn_deswizzle(&t->d, l, y, t->mem, cb, pitch)) { decoded_ok = 0; free(cb); RED_ONCE(9, "texture deswizzle failed for format %#x target %d", t->d.format, t->d.target); break; }
                if (nvn_bc2astc((uint32_t)t->d.format, cb, pitch, w, h, (uint8_t *)stg.contents + off, ap)) decoded_ok = 0;
                free(cb);
                up_blit_add(stg, off, ap, ap * ((h + 3) / 4), MTLSizeMake(w, h, 1), mt, y, l);
                off += ap * ((h + 3) / 4);
                continue;
            }
            if (unp) {   // No BC: first deswizzle into a temporary area (compressed blocks), then decompress layer by layer into the staging area
                size_t up = (size_t)w * ubpp;
                if (!cached.hit) {
                    uint8_t *cb = malloc(pitch * rows * z);
                    if (!cb || nvn_deswizzle(&t->d, l, y, decode_source, cb, pitch)) { decoded_ok = 0; free(cb); RED_ONCE(9, "texture deswizzle failed for format %#x target %d", t->d.format, t->d.target); break; }
                    for (uint32_t zi = 0; zi < z; zi++)
                        if (nvn_bc_decode(t->d.format, cb + zi * pitch * rows, pitch, w, h, decode_target + off + zi * up * h, up)) decoded_ok = 0;
                    free(cb);
                }
                up_blit_add(stg, off, up, up * h, MTLSizeMake(w, h, z), mt, y, l);
                off += up * h * z;
                continue;
            }
            if (nvn_deswizzle(&t->d, l, y, t->mem, (uint8_t *)stg.contents + off, pitch)) { RED_ONCE(9, "解排布失败 格式 %#x 目标 %d", t->d.format, t->d.target); break; }
            up_blit_add(stg, off, pitch, pitch * rows, MTLSizeMake(w, h, z), mt, y, l);
            off += pitch * rows * z;
        }
    }
    if (cached.entry) {
        // Staging owns its copy; later LRU eviction cannot change an in-flight
        // GPU read. No hash match or GPU-texture contents decide cache hits.
        memcpy(stg.contents, cached.decoded, total);
        nvn_cpu_decode_finish(&cached, decoded_ok);
    }
    if (unp && reupload) nvn_cpu_decode_report(atomic_load(&g_cur_frame));
    up_flush();
    return mt;
}
// Zero newly created non-packaged textures: a new Metal texture's content is undefined, while a freshly mapped memory pool on the Switch reads as 0. Games depend on it: SMBW's fountain water surface uses a 512×1 RGBA16F wave texture
// that every frame is "read last frame → render into a render target → copy back" and is never cleared; the undefined initial value keeps swinging through the feedback and pushes the water vertices to the top of the screen (the tall panels above the fountain).
// Copied level by level / slice by slice from one shared all-zero buffer, executed with the upload batch (ordered before this replay). Multisample, depth / stencil and block-compressed formats are not cleared (games always clear them before use, and they cannot be copied into anyway).
static size_t mtl_pf_bytes(MTLPixelFormat pf) {
    switch ((int)pf) {
        case 10: case 11: case 12: case 13: case 14: return 1;                                   // R8
        case 20: case 22: case 23: case 24: case 25: case 30: case 31: case 32: case 33: case 34: return 2;   // R16 / RG8
        case 40: case 41: case 42: case 43: return 2;                                            // packed 16-bit colour
        case 53: case 54: case 55: case 60: case 62: case 63: case 64: case 65:
        case 70: case 71: case 72: case 73: case 74: case 80: case 81:
        case 90: case 91: case 92: case 93: case 94: return 4;                                   // R32 / RG16 / RGBA8 / BGRA8 / 10-10-10-2 / 11-11-10 / 9-9-9-5
        case 103: case 104: case 105: case 110: case 112: case 113: case 114: case 115: return 8;   // RG32 / RGBA16
        case 123: case 124: case 125: return 16;                                                 // RGBA32
    }
    return 0;
}
// Shared all-zero buffer (for zeroing new textures and scissored zero clears): replaced by a bigger one when too small; the old one is released after in-flight submissions finish
static id<MTLBuffer> zero_buf(size_t need) {
    static id<MTLBuffer> zb; static size_t zn;
    if (need > zn) {
        size_t n = MAX(need, (size_t)4 << 20);
        id<MTLBuffer> b = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
        if (!b) return nil;
        memset(b.contents, 0, n); res_add(b);
        if (zb) defer_obj_release(zb);
        zb = b; zn = n;
    }
    return zb;
}
static id<MTLBuffer> astc_zero_buf(size_t need) {
    static id<MTLBuffer> zb; static size_t zn;
    if (need > zn) {
        size_t n = MAX(need, (size_t)1 << 20);
        id<MTLBuffer> b = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
        if (!b) return nil;
        uint8_t block[16]; nvn_astc_zero_block(block);
        for (size_t i = 0; i + sizeof block <= n; i += sizeof block) memcpy((uint8_t *)b.contents + i, block, sizeof block);
        res_add(b); if (zb) defer_obj_release(zb); zb = b; zn = n;
    }
    return zb;
}
static uint64_t g_tex_zeroed;
static void tex_zero_fill(id<MTLTexture> mt, int astc_bc) {
    static int warned;
    if (!atomic_load(&g_sub_thread_set) || !on_sub_thread()) return;   // the upload batch belongs to the submit thread only
    MTLTextureType tt = mt.textureType;
    if (tt == MTLTextureType2DMultisample || tt == MTLTextureType2DMultisampleArray || tt == MTLTextureTypeTextureBuffer) return;
    if (astc_bc && is_astc4(mt.pixelFormat)) {
        size_t need = (((size_t)mt.width + 3) / 4) * (((size_t)mt.height + 3) / 4) * 16;
        id<MTLBuffer> zb = astc_zero_buf(need); if (!zb) return;
        up_begin();
        NSUInteger slices = mt.arrayLength * (tt == MTLTextureTypeCube || tt == MTLTextureTypeCubeArray ? 6 : 1);
        for (NSUInteger l = 0; l < mt.mipmapLevelCount; l++) {
            NSUInteger w = MAX(mt.width >> l, 1u), h = MAX(mt.height >> l, 1u), nbx = (w + 3) / 4, nby = (h + 3) / 4;
            for (NSUInteger y = 0; y < slices; y++) up_blit_add_fill(zb, 0, nbx * 16, nbx * 16 * nby, MTLSizeMake(w, h, 1), mt, y, l);
        }
        [g_up_texs addObject:mt]; if (!g_cb) up_flush();
        return;
    }
    size_t bpp = mtl_pf_bytes(mt.pixelFormat);
    if (!bpp) { if (!warned++) MLOG("新建贴图清零：像素格式 %lu 不清（深度 / 模板 / 压缩等）", (unsigned long)mt.pixelFormat); return; }
    NSUInteger slices = mt.arrayLength * (tt == MTLTextureTypeCube || tt == MTLTextureTypeCubeArray ? 6 : 1);
    size_t need = (size_t)mt.width * bpp * mt.height * mt.depth;   // level 0, one slice is the largest
    if (need > (64u << 20)) { static int big; if (!big++) MLOG("新建贴图清零：%lux%lux%lu 超过 64 MB，不清", (unsigned long)mt.width, (unsigned long)mt.height, (unsigned long)mt.depth); return; }
    id<MTLBuffer> zb = zero_buf(need); if (!zb) return;
    up_begin();
    for (NSUInteger l = 0; l < mt.mipmapLevelCount; l++) {
        NSUInteger w = MAX(mt.width >> l, 1u), h = MAX(mt.height >> l, 1u), z = tt == MTLTextureType3D ? MAX(mt.depth >> l, 1u) : 1;
        for (NSUInteger y = 0; y < slices; y++) up_blit_add_fill(zb, 0, w * bpp, w * bpp * h, MTLSizeMake(w, h, z), mt, y, l);
    }
    [g_up_texs addObject:mt];
    if (!g_cb) up_flush();
    if (!g_tex_zeroed++) MLOG("新建贴图清零：开（Metal 新贴图内容未定义，Switch 新内存读 0）");
}

static int clear_rect_zero(int i);
// Linear textures (LINEAR) in a CPU-visible pool: the game writes the pool memory directly from the CPU (SMBW's glyph cache: 2048×8192 R8, IMAGE|LINEAR, pool CPU_UNCACHED|GPU_CACHED,
// the font engine rasterizes glyphs into it on the CPU). A GPU-private texture never sees those writes → all menu / dialog text is blank. On unified memory, create a linear texture directly on the pool buffer (zero copy): the GPU reads the current content at execution time, same as NVN.
static id<MTLTexture> tex_linear(const tex_t *t) {
    // LINEAR_RENDER_TARGET is a linear layout too (SMBW's sky colour ramps: four 64×1 RGBA16F, written by the CPU every frame)
    if (!(t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) || !t->mem || !t->pool || (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) return nil;
    if (t->d.target != NVN_TEXTURE_TARGET_2D || t->d.levels > 1 || t->d.samples > 1) return nil;
    uint32_t pf = 0; uint8_t swz[4]; int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!nvn_fmt_to_mtl(t->d.format, &pf, swz) || !pf || !bpb || bw != 1 || bh != 1) return nil;
    size_t row = t->d.stride ? t->d.stride : (size_t)t->d.width * bpb;
    void *mb; uint64_t mo;
    if (!mtl_addr((uint64_t)t->mem, (uint64_t)row * (t->d.height ? t->d.height : 1), &mb, &mo)) { RED_ONCE(64, "线性贴图的存储不在内存池里（%ux%u 格式 %#x）", t->d.width, t->d.height, t->d.format); return nil; }
    id<MTLBuffer> b = (__bridge id<MTLBuffer>)mb;
    NSUInteger al = [g_dev minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)pf];
    if (mo % al || row % al) { RED_ONCE(65, "线性贴图偏移 %#llx / 行距 %zu 不满足 %lu 字节对齐，按私有贴图建（CPU 写入不可见）", (unsigned long long)mo, row, (unsigned long)al); return nil; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)pf width:t->d.width ? t->d.width : 1 height:t->d.height ? t->d.height : 1 mipmapped:NO];
    d.resourceOptions = b.resourceOptions;
    d.usage = MTLTextureUsageShaderRead | (t->d.flags & NVN_TEXTURE_FLAGS_IMAGE ? MTLTextureUsageShaderWrite : 0) | (t->d.flags & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET ? MTLTextureUsageRenderTarget : 0);
    return [b newTextureWithDescriptor:d offset:mo bytesPerRow:row];
}
// CPU-written block-linear textures (WriteTexels): the game gave no packaged layout and changes the content from the CPU each frame (Engage's movie frames: a Y plane R8 + a UV plane RG8, WriteTexels every frame).
// Only packaged textures used to be uploaded from guest memory, so such a texture became a GPU-only texture whose content stayed 0. Their serials are recorded and they resolve through the packaged path (upload + re-upload on dirty pages)
static pthread_mutex_t g_cpu_tex_mu = PTHREAD_MUTEX_INITIALIZER; static CFMutableSetRef g_cpu_tex;
void mtl_tex_cpu_written(const tex_t *t) {
    uint64_t serial = t->serial;
    if (!serial) return;
    pthread_mutex_lock(&g_cpu_tex_mu);
    if (!g_cpu_tex) g_cpu_tex = CFSetCreateMutable(NULL, 0, NULL);
    int added = 0;
    if (!CFSetContainsValue(g_cpu_tex, (const void *)(uintptr_t)serial)) {
        CFSetAddValue(g_cpu_tex, (const void *)(uintptr_t)serial); added = 1;
        static int n; if (n++ < 16) MLOG("CPU 写贴图 ser=%llu %ux%ux%u 格式 %#x 目标 %d 级 %u：按预排布贴图上传 / 重传", (unsigned long long)serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels);
    }
    pthread_mutex_unlock(&g_cpu_tex_mu);
    if (added) { pthread_mutex_lock(&g_smu); g_ser_gen++; pthread_mutex_unlock(&g_smu); }   // the texture IDs that resolve to it take the slow path, which now treats it as packaged (tex_by_id view_pkg)
}
static int tex_cpu_written(uint64_t serial) {
    pthread_mutex_lock(&g_cpu_tex_mu); int r = g_cpu_tex && CFSetContainsValue(g_cpu_tex, (const void *)(uintptr_t)serial); pthread_mutex_unlock(&g_cpu_tex_mu);
    return r;
}
static id<MTLTexture> tex_mtl_(const tex_t *ct);
static id<MTLTexture> tex_mtl(const tex_t *ct) { AP_B; id<MTLTexture> r = tex_mtl_(ct); AP_E(4); return r; }
static uint64_t g_texinfo_ser[8]; static int g_texinfo_n;
static id<MTLTexture> tex_mtl_(const tex_t *ct) {
    tex_t *t = (tex_t *)ct;
    if (!t || !t->serial) return nil;
    if (t->magic != MAGIC('T', 'E', 'X', 0)) { RED_ONCE(70, "贴图对象已失效（magic %#x，首例 ser=%llu）：不建 MTLTexture", t->magic, (unsigned long long)t->serial); return nil; }
    int packaged = (t->has_layout || tex_cpu_written(t->serial) || linear_needs_upload(t, NULL, NULL)) && t->mem;
    {   // Diagnostic DM_NVN_TEXINFO=WxH: log storage and description of textures of that size on first resolution (to tell whether two textures share storage and why they were not merged)
        static int ti = -1; static uint32_t tw, th; static uint64_t last[64]; static int nl;
        if (ti > 0 && t->d.width == tw && t->d.height == th && g_texinfo_n < 8) { int dup = 0; for (int i = 0; i < g_texinfo_n; i++) dup |= g_texinfo_ser[i] == t->serial; if (!dup && !(t->d.flags & 8)) g_texinfo_ser[g_texinfo_n++] = t->serial; }   // remember the non-render-target ones (no COMPRESSIBLE) for DM_NVN_TEXRAW=info
        if (ti < 0) { const char *e = getenv("DM_NVN_TEXINFO"); ti = e && sscanf(e, "%ux%u", &tw, &th) == 2; }
        if (ti && t->d.width == tw && t->d.height == th) {
            int seen = 0; for (int i = 0; i < nl; i++) if (last[i] == t->serial) seen = 1;
            static uint64_t n; if (!seen && (n++ < 40 || n % 600 == 0)) { if (nl < 64) last[nl++] = t->serial; else last[n % 64] = t->serial;
                uint64_t nz = 0; if (t->mem && t->L.size <= (64u << 20)) for (uint64_t i = 0; i < t->L.size; i++) nz += t->mem[i] != 0;   // non-zero bytes in guest memory: content the CPU wrote
                MLOG("TEXINFO ser=%llu 预排布=%d has_layout=%d mem=%p pool=%p(%#x) off=%#llx 目标=%d 格式=%#x 标志=%#x 采样=%d %ux%ux%u 级=%u 跨距=%u 块高=%d 存储 %llu 字节 非零 %llu",
                     (unsigned long long)t->serial, packaged, t->has_layout, t->mem, t->pool, t->pool ? t->pool->flags : 0, (unsigned long long)t->off, t->d.target, t->d.format, t->d.flags, t->d.samples,
                     t->d.width, t->d.height, t->d.depth, t->d.levels, t->d.stride, t->d.pkg_block_h, (unsigned long long)t->L.size, (unsigned long long)nz); }
        }
    }
    id have = tex_get(t->serial);
    if (packaged) {
        NvPkg *pk = [have isKindOfClass:[NvPkg class]] ? have : nil;   // resolved as GPU-only before the CPU wrote it: dropped and rebuilt as packaged
        if (pk) {   // Already uploaded: check the covered pages only if there has been a guest write since the last check
            uint64_t now = nvn_dirty_now();
            int tracked = pk->span != NULL;   // kept from the upload: a texture's pool stays registered while the texture lives
            int clean = tracked && (now == pk->seen || !nvn_dirty_since_span(pk->span, t->mem, t->L.size, pk->stamp));
            pk->seen = now;
            if (clean && atomic_load_explicit(&g_up_lost_n, memory_order_relaxed) && up_lost_take(pk->tex)) { clean = 0; uint64_t k = atomic_load(&g_up_lost_redo); if (k <= 3 || !atomic_load(&g_up_lost_n)) MLOG("上传被丢的贴图 ser=%llu 重传（累计 %llu 张，记录里还剩 %d 张）", (unsigned long long)t->serial, (unsigned long long)k, atomic_load(&g_up_lost_n)); }
            if (clean) {
                if (dirtycheck() && full_hash(t->mem, t->L.size) != pk->hash && !g_pkg_miss++)
                    MLOG("【红】脏页漏检：贴图 ser=%llu 池内容已变但未检出", t->serial);
                return pk->tex;
            }
            // Pool content changed (streamed assets written into the same storage, a movie frame): switch to another texture, as in-flight draws still read the old one. That is the spare
            // when the GPU is done with it (see NvPkg), else a new one; the old one becomes the spare (pkg_upload_ made nt resident)
            id<MTLTexture> reuse = nil;
            if (pk->spare) { pthread_mutex_lock(&g_mu); uint64_t done = g_done; pthread_mutex_unlock(&g_mu); if (pk->spare_fence <= done && pk->spare.usage == pk->tex.usage) reuse = pk->spare; }
            uint64_t st; id<MTLTexture> nt = pkg_upload(t, &st, reuse, 1);
            if (nt) { id<MTLTexture> old = pk->tex; pk->tex = nt;
                if (pk->spare && pk->spare != nt) defer_obj_release(pk->spare);   // still in use, or of another shape: retired as before
                // A write may fault after deswizzling read its source but before
                // this upload returns. Keep the checked generation at the
                // upload stamp so the next lookup cannot hide that write.
                pk->spare = old; pk->spare_fence = g_fence + 1; g_ser_gen++; pk->stamp = st; pk->span = nvn_dirty_span(t->mem, t->L.size); pk->seen = st; g_pkg_reuploads++; if (dirtycheck()) pk->hash = full_hash(t->mem, t->L.size); }
            return pk->tex;
        }
        pk = [NvPkg new];
        pk->tex = pkg_upload(t, &pk->stamp, nil, 0);
        if (!pk->tex) return nil;
        pk->span = nvn_dirty_span(t->mem, t->L.size);
        pk->seen = pk->stamp; g_pkg_uploads++; if (dirtycheck()) pk->hash = full_hash(t->mem, t->L.size);
        tex_put(t->serial, pk);
        return pk->tex;
    }
    if (have) return have;
    if (t->d.target == 10) {   // Buffer texture (TARGET_BUFFER): create the texture-buffer view directly on the memory pool buffer (zero copy; the GPU reads the pool's current content at execution time, same as NVN semantics)
        uint32_t pf = 0; uint8_t swz[4]; int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
        void *mb; uint64_t mo;
        if (!nvn_fmt_to_mtl(t->d.format, &pf, swz) || !pf || !bpb) { RED_ONCE(36, "缓冲贴图格式 %#x 未映射", t->d.format); return nil; }
        if (!t->mem || !mtl_addr((uint64_t)t->mem, (uint64_t)t->d.width * bpb, &mb, &mo)) { RED_ONCE(37, "缓冲贴图的存储不在内存池里"); return nil; }
        id<MTLBuffer> b = (__bridge id<MTLBuffer>)mb;
        NSUInteger al = [g_dev minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)pf];
        if (mo % al) { RED_ONCE(38, "缓冲贴图偏移 %#llx 不满足 %lu 字节对齐", (unsigned long long)mo, (unsigned long)al); return nil; }
        MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:(MTLPixelFormat)pf width:t->d.width resourceOptions:b.resourceOptions
                                                                                         usage:MTLTextureUsageShaderRead | (t->d.flags & NVN_TEXTURE_FLAGS_IMAGE ? MTLTextureUsageShaderWrite : 0)];
        size_t row = ((size_t)t->d.width * bpb + al - 1) / al * al;
        id<MTLTexture> mt = [b newTextureWithDescriptor:d offset:mo bytesPerRow:row];
        if (!mt) { RED_ONCE(39, "缓冲贴图建不出：%u 元素 格式 %#x", t->d.width, t->d.format); return nil; }
        res_add(mt);
        tex_put(t->serial, mt);
        return mt;
    }
    alias_key ak; memset(&ak, 0, sizeof ak); ak.pool = (uint64_t)(uintptr_t)t->pool; ak.off = t->off; ak.d = t->d;
    pthread_mutex_lock(&g_smu); __unsafe_unretained id shared = hm_get_id(&g_alias, &ak); pthread_mutex_unlock(&g_smu);
    if (shared) { tex_put(t->serial, shared); return shared; }
    id<MTLTexture> mt = tex_linear(t);
    if (!mt) {
        MTLTextureDescriptor *d = tex_desc(t, 0);
        if (!d) return nil;
        mt = [g_dev newTextureWithDescriptor:d];
        if (!mt) { RED_ONCE(2, "建 MTLTexture 失败 %ux%u 格式 %#x 目标 %d", t->d.width, t->d.height, t->d.format, t->d.target); return nil; }
        tex_tag_format(mt, t->d.format);
        res_add(mt); { uint32_t apf; tex_zero_fill(mt, bc_astc(t, &apf)); }   // register residency before zeroing (the copy destination must be resident too)
    }
    tex_tag_format(mt, t->d.format);
    if (g_allocprof > 0) { static int n; if (n++ < 30 || n % 500 == 0) MLOG("新建贴图（%s）#%d ser=%llu %ux%ux%u 格式 %#x 目标 %d 级 %u 存储 %p", __func__, n, (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, (void *)t->mem); }
    if (getenv("DM_NVN_TEXCPU") && t->pool && !(t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) MLOG("CPU 可见池上的非预排布贴图 ser=%llu %ux%ux%u 格式 %#x 目标 %d 级 %u 贴图标志 %#x 池标志 %#x 存储 %p", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, t->d.flags, t->pool->flags, (void *)t->mem);
    res_add(mt);
    pthread_mutex_lock(&g_smu);
    if (!g_alias_set) g_alias_set = CFSetCreateMutable(NULL, 0, NULL);
    hm_put_id(&g_alias, &ak, mt); CFSetAddValue(g_alias_set, (__bridge const void *)mt);
    pthread_mutex_unlock(&g_smu);
    tex_put(t->serial, mt);
    return mt;
}
// Live texture memory, bucketed by the guest NVN source format and the actual
// Metal storage. This is emitted at ordinary memory samples and upload-batch
// diagnostics so a jetsam between frame samples still leaves useful evidence.
static const void *const k_tex_fmt_key = &k_tex_fmt_key;
static void tex_tag_format(id<MTLTexture> mt, uint32_t nvn_fmt) {
    if (mt) objc_setAssociatedObject(mt, k_tex_fmt_key, @(nvn_fmt), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
static int tex_src_class(uint32_t f) {
    switch (f) {
        case 0x41: case 0x42: case 0x45: case 0x46: return 1;
        case 0x43: case 0x47: return 2;
        case 0x44: case 0x48: return 3;
        case 0x49: case 0x4a: return 4;
        case 0x4b: case 0x4c: return 5;
        case 0x4f: case 0x50: return 6;
        case 0x4d: case 0x4e: return 7;
        default: return 0;
    }
}
static void tex_format_stat(void) {
    enum { TEX_SRC_CLASSES = 8, TEX_STORAGE_CLASSES = 4 };
    static const char *const src[] = {"non-BC", "BC1", "BC2", "BC3", "BC4", "BC5", "BC6H", "BC7"};
    static const char *const dst[] = {"as-is", "native-BC", "ASTC", "decompressed"};
    _Static_assert(sizeof src / sizeof *src == TEX_SRC_CLASSES, "texture source bucket count");
    _Static_assert(sizeof dst / sizeof *dst == TEX_STORAGE_CLASSES, "texture storage bucket count");
    uint64_t n[TEX_SRC_CLASSES][TEX_STORAGE_CLASSES] = {{0}}, bytes[TEX_SRC_CLASSES][TEX_STORAGE_CLASSES] = {{0}};
    uint64_t tex_bytes = 0, untagged_bytes = 0, buffer_bytes = 0, other_bytes = 0;
    uint64_t tex_n = 0, untagged_n = 0, buffer_n = 0, other_n = 0;
    @autoreleasepool {
        // Take one strong point-in-time snapshot while holding only the
        // residency lock. Classification and logging stay outside the lock:
        // they may call Objective-C runtime / Metal accessors and must not
        // introduce a g_res_mu -> logging or cache-lock ordering edge.
        pthread_mutex_lock(&g_res_mu);
        NSArray *roots = g_res_objects ? g_res_objects.allObjects : @[];
        pthread_mutex_unlock(&g_res_mu);

        CFMutableSetRef seen_tex = CFSetCreateMutable(NULL, 0, NULL);
        CFMutableSetRef seen_buf = CFSetCreateMutable(NULL, 0, NULL);
        for (id<MTLResource> resource in roots) {
            if ([resource conformsToProtocol:@protocol(MTLBuffer)]) {
                const void *key = (__bridge const void *)resource;
                if (!CFSetContainsValue(seen_buf, key)) {
                    CFSetAddValue(seen_buf, key); buffer_n++; buffer_bytes += resource.allocatedSize;
                }
                continue;
            }
            if (![resource conformsToProtocol:@protocol(MTLTexture)]) {
                other_n++; other_bytes += resource.allocatedSize;
                continue;
            }

            // Views are not allocations. Canonicalize any accidentally
            // registered view to its root so a base + view pair is counted
            // once. A buffer-backed texture is accounted as its root buffer.
            id<MTLTexture> mt = (id<MTLTexture>)resource;
            while (mt.parentTexture) mt = mt.parentTexture;
            if (mt.buffer) {
                id<MTLBuffer> buffer = mt.buffer;
                const void *key = (__bridge const void *)buffer;
                if (!CFSetContainsValue(seen_buf, key)) {
                    CFSetAddValue(seen_buf, key); buffer_n++; buffer_bytes += buffer.allocatedSize;
                }
                continue;
            }
            const void *key = (__bridge const void *)mt;
            if (CFSetContainsValue(seen_tex, key)) continue;
            CFSetAddValue(seen_tex, key);

            uint64_t sz = mt.allocatedSize; tex_n++; tex_bytes += sz;
            NSNumber *fmt = objc_getAssociatedObject(mt, k_tex_fmt_key);
            if (!fmt) { untagged_n++; untagged_bytes += sz; continue; }
            int c = tex_src_class(fmt.unsignedIntValue);
            int st = !c ? 0 : is_native_bc(mt.pixelFormat) ? 1 : is_astc4(mt.pixelFormat) ? 2 : 3;
            n[c][st]++; bytes[c][st] += sz;
        }
        CFRelease(seen_tex); CFRelease(seen_buf);
    }
    char line[896]; int at = snprintf(line, sizeof line, "%llu resident root textures, %.1f MB:", (unsigned long long)tex_n, tex_bytes / 1048576.0);
    for (int c = 0; c < TEX_SRC_CLASSES && at < (int)sizeof line - 112; c++) for (int st = 0; st < TEX_STORAGE_CLASSES && at < (int)sizeof line - 112; st++) if (n[c][st])
        at += snprintf(line + at, sizeof line - at, " %s/%s %llu textures %.1f MB;", src[c], dst[st], (unsigned long long)n[c][st], bytes[c][st] / 1048576.0);
    snprintf(line + at, sizeof line - at, " untagged textures %llu %.1f MB; root buffers %llu %.1f MB; other resources %llu %.1f MB",
             (unsigned long long)untagged_n, untagged_bytes / 1048576.0,
             (unsigned long long)buffer_n, buffer_bytes / 1048576.0,
             (unsigned long long)other_n, other_bytes / 1048576.0);
    MLOG("texture memory by format: %s", line);
}
// Memory readout line (always on, low frequency, capped): resident memory, GPU allocated, deferred-release queue, residency set object count, registered memory pools / peak / tracking table capacity, cumulative texture re-uploads.
// Purpose: pin down "crashes with no log". The system gives no notice before killing a process for memory, so only readouts left beforehand can show which number kept climbing.
// Called by the frame hook every 300 frames; printed only when resident or GPU allocated changed by ≥ 64 MB since the last line, otherwise one line every 3000 frames; capped at 400 lines per game. All integers (the frame hook runs on the guest thread's small stack, so no floating-point formatting)
void mtl_mem_stat(unsigned long long frame, unsigned long long rss_mb) {
    static unsigned long long last_rss, last_gpu, last_frame; static int lines; unsigned long long gpu = (unsigned long long)(g_dev.currentAllocatedSize >> 20);
    unsigned long long dr = rss_mb > last_rss ? rss_mb - last_rss : last_rss - rss_mb, dg = gpu > last_gpu ? gpu - last_gpu : last_gpu - gpu;
    if (lines >= 400 || (lines && dr < 64 && dg < 64 && frame - last_frame < 3000)) return; lines++; last_rss = rss_mb; last_gpu = gpu; last_frame = frame;
    pthread_mutex_lock(&g_deferred_mu); unsigned long nd = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    pthread_mutex_lock(&g_res_mu); unsigned long nr = (unsigned long)g_res_objects.count; pthread_mutex_unlock(&g_res_mu);
    int live = 0, peak = 0, cap = 0; nvn_dirty_stat(&live, &peak, &cap);
    pthread_mutex_lock(&g_smu); unsigned ns = g_by_serial.n, ashared = g_alias.n, nghost = g_ghost.n; uint64_t ghost_puts = g_ghost_puts; pthread_mutex_unlock(&g_smu);
    unsigned long dead_n = 0; unsigned long long dead_mb = 0;
    // Objects read from a weak table come back retained + autoreleased: the frame hook runs on a guest thread with no autorelease pool that drains, so without this wrapper every buffer counted would be pinned for good (the readout creating the leak itself)
    int pres = 0; @autoreleasepool { pthread_mutex_lock(&g_mu); pres = g_pool_res_n; for (id<MTLBuffer> b in g_dead_pools) { dead_n++; dead_mb += b.length; } pthread_mutex_unlock(&g_mu); } dead_mb >>= 20;   // host objects held by serial (textures / packaged textures / programs): if it climbs along with GPU allocated, textures are not being released
    // os_proc_available_memory: how far this process is from being terminated by the system for memory, iOS only (from d2r-mobile 704147218); the Mac writes 「无」(none). On a phone, a crash with no log is most likely this number reaching 0
    char avail[32] = "无";
#if TARGET_OS_IPHONE
    snprintf(avail, sizeof avail, "%llu MB", (unsigned long long)(os_proc_available_memory() >> 20));
#endif
    MLOG("内存 至帧 %llu：常驻 %llu MB，显卡已分配 %llu MB，延迟释放队列 %lu，驻留集 %lu 个，序号对象 %u 个，内存池 %d 个（其中已驻留 %d；峰值 %d / 跟踪表容量 %d；已释放仍活 %lu 个 %llu MB），共用贴图 %u 张（随池淘汰累计 %llu），贴图重传累计 %llu 次，每帧竞技场累计新建 %llu 块，已 Finalize 贴图小表 %u 张（累计 %llu）；离系统内存上限还有 %s", frame, rss_mb, gpu, nd, nr, ns, live, pres, peak, cap, dead_n, dead_mb, ashared, (unsigned long long)g_alias_evicted, (unsigned long long)g_pkg_reuploads, (unsigned long long)g_arena_made, nghost, (unsigned long long)ghost_puts, avail);
    tex_format_stat();
    if (g_allocprof > 0) {
        MLOG("显卡分配增长按函数：结束通道 %llu MB，开通道 %llu MB，取管线 %llu MB，参数缓冲 %llu MB，取贴图 %llu MB", g_ap_fn[0] >> 20, g_ap_fn[1] >> 20, g_ap_fn[2] >> 20, g_ap_fn[3] >> 20, g_ap_fn[4] >> 20); }
    if (g_allocprof > 0) { int top[4] = {-1, -1, -1, -1}; for (int o = 0; o < 64; o++) for (int k = 0; k < 4; k++) if (top[k] < 0 || g_ap_bytes[o] > g_ap_bytes[top[k]]) { for (int j = 3; j > k; j--) top[j] = top[j - 1]; top[k] = o; break; }
        MLOG("显卡分配增长按操作：op%d %llu MB，op%d %llu MB，op%d %llu MB，op%d %llu MB", top[0], g_ap_bytes[top[0]] >> 20, top[1], g_ap_bytes[top[1]] >> 20, top[2], g_ap_bytes[top[2]] >> 20, top[3], g_ap_bytes[top[3]] >> 20); }
}
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = g_pkg_uploads; *reuploads = g_pkg_reuploads; }
uint64_t mtl_pkg_miss(void) { return g_pkg_miss; }
void mtl_tex_release(tex_t *t) {
    if (!t->serial) return;
    pthread_mutex_lock(&g_smu); nvn_hm_del(&g_tex_live, &t->serial); pthread_mutex_unlock(&g_smu);
    serial_del(t->serial);
}

// ---------- Render passes: SetRenderTargets opens a new pass; clears before the first command in the pass are folded into the load action ----------
static struct {
    p_set_rt rt; int has_rt;
    uint8_t clear_c[8]; MTLClearColor cc[8];
    uint8_t clear_d, clear_s; double cd; uint32_t cs;
    p_scissor sc; int has_sc;
} R;

// ---------- Zero-source skip ----------
// Shader fact table (statically proven keys): 'the textures it samples are all 0 and the coefficient is finite ⇒ the output is all 0'. Coefficient = component comp of vector vec of fragment-stage constant buffer register ubo_slot.
//   4a819818b8e489c5 ps_outline_blur_p: output = (sum of several samples of the same texture along a row / column) × fp_c7._m0[1].x (fp_c7 = cbuf reg 4), homogeneous linear in the sampled values, no constant term
//   (source of the proof: dmsh_b/<chain version>/4a819818b8e489c5_b.metal, read line by line). Add other shaders here once they are proven.
typedef struct { char key[17]; int ubo_slot, vec, comp; } zskip_fact;
static zskip_fact g_zskip_facts[16] = {{"4a819818b8e489c5", 4, 1, 0}};
static int g_zskip_nf = 1;
void zskip_test_add(const char *key, int ubo_slot, int vec, int comp) {
    if (!key || g_zskip_nf >= 16) return;
    snprintf(g_zskip_facts[g_zskip_nf].key, sizeof g_zskip_facts[0].key, "%s", key);
    g_zskip_facts[g_zskip_nf].ubo_slot = ubo_slot; g_zskip_facts[g_zskip_nf].vec = vec; g_zskip_facts[g_zskip_nf].comp = comp; g_zskip_nf++;
}
int zskip_test_count(void) { return g_zskip_count; }
static int g_zskip_env = -1;   // Cache of DM_NVN_ZSKIP (−1 = not read). This function is called on every draw, and getenv once took about 8% of the submit path; unit tests set it back to −1 after toggling the switch mid-run
static int zskip_on(void) { if (g_zskip_env < 0) { const char *e = getenv("DM_NVN_ZSKIP"); g_zskip_env = !e || strcmp(e, "0"); } return g_zskip_env && tbdr_on(); }
static void zskip_clear(void) { g_zskip_n = 0; memset(g_zskip_cand_ok, 0, sizeof g_zskip_cand_ok); }
static void zskip_del(uint64_t ser) { for (int i = 0; i < g_zskip_n;) if (g_zskip_zero[i].ser == ser) g_zskip_zero[i] = g_zskip_zero[--g_zskip_n]; else i++; }
static int zskip_has(uint64_t ser, int lv, int sl) { for (int i = 0; i < g_zskip_n; i++) if (g_zskip_zero[i].ser == ser && g_zskip_zero[i].level == lv && g_zskip_zero[i].slice == sl) return 1; return 0; }
static void zskip_add(uint64_t ser, int lv, int sl) { if (!ser || zskip_has(ser, lv, sl) || g_zskip_n >= 4096) return; g_zskip_zero[g_zskip_n++] = (zskip_zero){ser, (uint16_t)lv, (uint16_t)sl}; }
static int zskip_zero_clear(int i) { return R.clear_c[i] && R.cc[i].red == 0 && R.cc[i].green == 0 && R.cc[i].blue == 0 && R.cc[i].alpha == 0; }
static void zskip_commit_clears(void) { for (int i = 0; i < 8; i++) if (g_zskip_cand_ok[i]) zskip_add(g_zskip_cand[i].ser, g_zskip_cand[i].level, g_zskip_cand[i].slice); }
static int zskip_target_zero(const tex_t *t, const view_t *v, int hv) { int lv = hv && v->set_levels ? v->base_level : 0, sl = hv && v->set_layers ? v->min_layer : 0; return t && zskip_has(t->serial, lv, sl); }
#ifdef NVN_RT_RANGE_TEST
static int g_rr_checking;
static void rr_begin(void), rr_finish(int ranged);
static void rr_snap(const p_draw *, const p_draw_idx *);
static void rr_reset(void);
#endif
static id<MTLRenderCommandEncoder> g_enc;
static int enc_open(void) { return g_enc != nil; }
// Target info of the current render pass: determined once from the attachments at rp_begin and shared by all draws in the pass (pipeline key, depth-stencil, viewport / scissor all depend only on it)
static struct { uint32_t cfmt[8], dfmt, sfmt, samples; int hd, hs; NSUInteger tw, th; uint64_t serial, reg_gen, ser_gen, range_dirty; int range_external, range_depth_nonro; __unsafe_unretained id<MTLTexture> range_color, range_depth; } g_pass;
static id<MTLRenderPipelineState> g_enc_pso; static id<MTLDepthStencilState> g_enc_dss;   // Already set on this encoder (not set again if identical)
// Dynamic state already sent on the open render encoder: each draw used to send all nine (stencil reference, viewport, scissor, cull, winding, fill, depth bias, depth clip, blend colour);
// now a value is sent only when it differs. g_ed.ok holds one bit per item and is cleared for every new encoder and after a clear triangle (which sets its own)
static struct { unsigned ok; uint32_t sref; MTLViewport vp; MTLScissorRect sc; int cull, wind, fill, clip; float bias[3], blend[4]; } g_ed;
enum { ED_SREF = 1, ED_VP = 2, ED_SC = 4, ED_CULL = 8, ED_WIND = 16, ED_FILL = 32, ED_BIAS = 64, ED_CLIP = 128, ED_BLEND = 256 };
static int g_enc_vis, g_enc_has_vis;   // This encoder's current visibility count mode (0 off / slot number + 1), and whether a visibility buffer is attached
static int g_pso_dirty = 1, g_dss_dirty = 1;   // Pipeline / depth-stencil related state was changed during replay
static uint64_t g_stat_args_enc, g_stat_args_reuse;   // Argument buffer: new-encode / reuse counts
static uint64_t g_stat_draw, g_stat_pass, g_bw_load, g_bw_store, g_pass_full_rw, g_udc_saved_bytes;   // Bandwidth ledger: attachment load / store bytes, number of full load-store passes
// DM_NVN_PASSDUMP: records pass attachments / samples / presents one by one (textures by serial) for offline data-flow analysis; the pass numbers of P / D / R / S lines agree (= g_stat_pass after the pass is opened)
// DM_NVN_PASSDUMP=frame number, or start-end (inclusive at both ends)
static int g_kd_force;   // the draw DM_NVN_KEYDUMP matched: force the per-binding records on (see kd_draw)
static _Atomic uint64_t g_passdump_live; // Upper 56 bits: start frame; lower 8 bits: count.
static int passdump(void) {
    if (g_kd_force) return 1;
#ifdef NVN_RT_RANGE_TEST
    if (g_rr_checking) return 0;
#endif
    uint64_t live = atomic_load_explicit(&g_passdump_live, memory_order_acquire);
    if (live) {
        uint64_t frame = atomic_load(&g_cur_frame), start = live >> 8, count = live & 255;
        if (frame >= start && frame - start < count) return 1;
    }
    static long a0 = -2, a1; if (a0 == -2) { const char *e = getenv("DM_NVN_PASSDUMP"); a0 = e ? atol(e) : -1; a1 = e && strchr(e, '-') ? atol(strchr(e, '-') + 1) : a0; }
    long f = (long)atomic_load(&g_cur_frame); return a0 >= 0 && f >= a0 && f <= a1;
}
#define PD(...) do { if (passdump()) { fprintf(stderr, "[pass] F%llu ", (unsigned long long)atomic_load(&g_cur_frame)); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static void blit_note(void) { if (g_enc) PD("E %llu 拷贝 / 缓冲更新结束渲染通道", g_stat_pass); }
// DM_GFXUSE=<file>: once the character select screen is stable (starting 1 second after the first OpenPanel MainMenuButtonRibbon, requires DM_BOOTPROF=1), over 30 consecutive frames,
// the ranges in guest memory of the resources the GPU actually reads (texture storage, UBO / SSBO, vertex / index buffers), deduplicated by (kind, address, length), one line recorded on first sight.
// For intersecting with the inflate output ranges (which data decompressed before character select is really used by the character select screen). Columns: kind  guest_addr  len  first_frame  tex_id  fmt  w×h
__attribute__((weak)) uint64_t dm_prof_ribbon_ns(void) { return 0; }   // diag.c provides the strong definition; when not merged it is always 0 → the time window does not open, and a failure is reported at the end
__attribute__((weak)) void dm_shot_written(const char *path, int ok) { (void)path; (void)ok; }
__attribute__((weak)) void dm_guest_frame_tick(void) {}   // hle_os.c provides the strong definition (DM_DETCLOCK deterministic guest clock); a no-op when graphics unit tests do not link the host   // diag.c provides the strong definition (DM_TICK_HOLD=E:SHOT unfreeze); a no-op when graphics unit tests do not link the host
__attribute__((weak)) int dm_detclock_enabled(void) { return 0; }   // Same as above: the host's hle_os.c provides the strong definition; when graphics unit tests do not link the host, the deterministic clock is treated as off
__attribute__((weak)) uint64_t guest_ns(void) { return 0; }          // Same as above: read only when the deterministic clock is on; never reached in graphics unit tests
enum { USE_OFF, USE_WAIT, USE_REC, USE_DONE };
static int g_use = -1; static FILE *g_use_f; static long g_use_f0, g_use_rows; static nvn_hmap g_use_seen = {.klen = 24};
#define USE_FRAMES 30
static void use_exit(void) {
    if (g_use == USE_WAIT) fprintf(stderr, "[dm] 【红】DM_GFXUSE 时间窗始终没开：dm_prof_ribbon_ns() = %llu（需 DM_BOOTPROF=1 且宿主含面板打点）\n", (unsigned long long)dm_prof_ribbon_ns());
    else if (g_use == USE_REC) fprintf(stderr, "[dm] 【红】DM_GFXUSE 只记了 %ld 帧（不足 %d）即退出，文件未完整\n", (long)atomic_load(&g_cur_frame) - g_use_f0, USE_FRAMES);
    if (g_use_f) fflush(g_use_f);
}
static void use_tick(void) {   // Called on every present
    if (g_use < 0) { const char *e = getenv("DM_GFXUSE"); g_use = USE_OFF;
        if (e) { g_use_f = fopen(e, "w"); if (!g_use_f) { MLOG("【红】DM_GFXUSE 打不开 %s", e); return; }
            fprintf(g_use_f, "kind\tguest_addr\tlen\tfirst_frame\ttex_id\tfmt\twxh\n"); fflush(g_use_f); g_use = USE_WAIT; atexit(use_exit);
            MLOG("[gfxuse] 等面板事件：首次 OpenPanel MainMenuButtonRibbon 后 1 秒起记 %d 帧 → %s", USE_FRAMES, e); } }
    long f = (long)atomic_load(&g_cur_frame);
    if (g_use == USE_WAIT) {
        uint64_t t = dm_prof_ribbon_ns();
        if (t && now_ns() - t >= 1000000000ull) { g_use = USE_REC; g_use_f0 = f; MLOG("[gfxuse] 开始记录：帧 %ld 起 %d 帧", f, USE_FRAMES); }
    } else if (g_use == USE_REC && f - g_use_f0 >= USE_FRAMES) {
        g_use = USE_DONE; fclose(g_use_f); g_use_f = NULL;
        MLOG("[gfxuse] 记录完成：帧 %ld–%ld，去重后 %ld 个区间", g_use_f0, f - 1, g_use_rows);
    }
}
static __attribute__((always_inline)) inline void use_rec(const char *kind, uint64_t addr, uint64_t len, int tid, const tex_t *t) {
    if (g_use != USE_REC || !addr || !len) return;
    uint64_t k[3] = {addr, len, (uint64_t)(uint8_t)kind[0]};
    if (nvn_hm_get(&g_use_seen, k)) return;
    nvn_hm_put(&g_use_seen, k, (void *)1);
    g_use_rows++;
    if (t) fprintf(g_use_f, "%s\t%#llx\t%llu\t%lld\t%d\t%#x\t%ux%u\n", kind, (unsigned long long)addr, (unsigned long long)len, (long long)atomic_load(&g_cur_frame), tid, t->d.format, t->d.width, t->d.height);
    else fprintf(g_use_f, "%s\t%#llx\t%llu\t%lld\t\t\t\n", kind, (unsigned long long)addr, (unsigned long long)len, (long long)atomic_load(&g_cur_frame));
}
static uint64_t att_bytes(id<MTLTexture> t, NSUInteger level) {
    if (!t) return 0;
    NSUInteger w = MAX(t.width >> level, 1), h = MAX(t.height >> level, 1);
    NSUInteger bpp = t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? 5 : t.pixelFormat == MTLPixelFormatRGBA16Float || t.pixelFormat == MTLPixelFormatRG32Float ? 8 :
                     t.pixelFormat == MTLPixelFormatRGBA32Float ? 16 : t.pixelFormat == MTLPixelFormatR8Unorm || t.pixelFormat == MTLPixelFormatStencil8 ? 1 :
                     t.pixelFormat == MTLPixelFormatRG8Unorm || t.pixelFormat == MTLPixelFormatR16Float ? 2 : 4;
    return (uint64_t)w * h * bpp * t.sampleCount;
}
// The render attachment selects a particular mip, while MTLTexture.width/height still describe the base level. Empty attachments do not take part in the bounds.
static NSUInteger mip_extent(NSUInteger size, NSUInteger level) {
    return level < sizeof(size) * 8 ? MAX(size >> level, 1) : 1;
}
static void pass_dims(MTLRenderPassDescriptor *pd, NSUInteger nc, NSUInteger *w, NSUInteger *h) {
    *w = *h = 0;
    for (NSUInteger i = 0; i < nc + 2; i++) {
        MTLRenderPassAttachmentDescriptor *a = i < nc ? pd.colorAttachments[i] : i == nc ? pd.depthAttachment : pd.stencilAttachment;
        if (!a.texture) continue;
        NSUInteger aw = mip_extent(a.texture.width, a.level), ah = mip_extent(a.texture.height, a.level);
        *w = *w ? MIN(*w, aw) : aw; *h = *h ? MIN(*h, ah) : ah;
    }
    if (!*w) { *w = MAX(pd.renderTargetWidth, 1); *h = MAX(pd.renderTargetHeight, 1); }
}

// ---------- DM_NVN_PASSGPU=<file>: per-pass GPU time (stage-boundary timestamp counters) ----------
// Each render pass samples 4 points (vertex start/end, fragment start/end), a compute pass 2 points; resolved after the command buffer completes and accumulated by 'index of the pass within the frame'.
// Each line: frame idx kind gpu_ms inc_ms w h fmt0 dfmt load draws t0_ns t1_ns (kind: R render / C compute; load: L load / C clear for each color attachment + depth)
// On TBDR passes overlap in the pipeline (the later pass's vertex stage is earlier than the end of the earlier pass's fragment stage): gpu_ms = this pass's start-to-end span (including queueing) and cannot be summed;
// attribution uses the timeline increment inc_ms = t1 − max(t0, latest t1 of the passes completed so far); the sum of the increments of all passes = total time the GPU is busy (command buffers complete in submission order)
typedef struct { uint64_t frame, pass; int idx, kind, w, h, draws, slot; uint32_t cfmt0, dfmt; char load[12]; } pg_pass;   // pass: global pass number (same as the pass number in the DM_NVN_PASSDUMP lines, for joining the two records)
#define PG_SLOTS 4096   // Counter sample buffer limit 32768 bytes = 4096 timestamps (measured from the error on this machine)
#define PG_RING 8
static int g_pg = -1; static FILE *g_pg_f;
static id<MTLCounterSampleBuffer> g_pg_buf[PG_RING]; static int g_pg_b; static uint32_t g_pg_used;
static pg_pass *g_pg_pass; static int g_pg_np, g_pg_cap, g_pg_open = -1;
static uint64_t g_pg_frame = ~0ull; static int g_pg_idx;
static pthread_mutex_t g_pg_mu = PTHREAD_MUTEX_INITIALIZER;
static inline void args_dirty_all(void);
typedef struct { double sum, last; uint64_t n; pg_pass meta; } pg_stat;
static pg_stat g_pg_st[256]; static uint64_t g_pg_rep_frame;
static int pg_on(void) {
    if (g_pg >= 0) return g_pg;
    const char *e = getenv("DM_NVN_PASSGPU"); g_pg = 0;
    if (!e) return 0;
    if (![g_dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) { MLOG("【红】DM_NVN_PASSGPU：本设备不支持阶段边界计数采样"); return 0; }
    id<MTLCounterSet> ts = nil; for (id<MTLCounterSet> c in g_dev.counterSets) if ([c.name isEqualToString:MTLCommonCounterSetTimestamp]) ts = c;
    if (!ts) { MLOG("【红】DM_NVN_PASSGPU：没有时间戳计数器集"); return 0; }
    MTLCounterSampleBufferDescriptor *d = [MTLCounterSampleBufferDescriptor new];
    d.counterSet = ts; d.storageMode = MTLStorageModeShared; d.sampleCount = PG_SLOTS;
    for (int i = 0; i < PG_RING; i++) { NSError *er = nil; g_pg_buf[i] = [g_dev newCounterSampleBufferWithDescriptor:d error:&er]; if (!g_pg_buf[i]) { MLOG("【红】计数采样缓冲建不出：%s", er.localizedDescription.UTF8String); return 0; } }
    if (!(g_pg_f = fopen(e, "w"))) { MLOG("【红】DM_NVN_PASSGPU 打不开 %s", e); return 0; }
    fprintf(g_pg_f, "frame\tidx\tkind\tgpu_ms\tinc_ms\tw\th\tfmt0\tdfmt\tload\tdraws\tt0_ns\tt1_ns\tpass\n");
    MLOG("[passgpu] 逐通道 GPU 计时 → %s", e);
    return g_pg = 1;
}
static void pg_sub_begin(uint64_t fence) {   // This submission uses block fence % PG_RING of the ring; first wait for the submission that used it PG_RING submissions ago to complete
    if (!pg_on()) return;
    if (fence > PG_RING) mtl_wait(fence - PG_RING, UINT64_MAX);
    g_pg_b = (int)(fence % PG_RING); g_pg_used = 0; g_pg_np = 0; g_pg_open = -1;
}
static pg_pass *pg_new(int kind, int nslot) {
    if (!pg_on() || g_pg_used + nslot > PG_SLOTS) return NULL;
    uint64_t f = atomic_load(&g_cur_frame); if (f != g_pg_frame) { g_pg_frame = f; g_pg_idx = 0; }
    if (g_pg_np == g_pg_cap) { g_pg_cap = g_pg_cap ? g_pg_cap * 2 : 128; g_pg_pass = realloc(g_pg_pass, g_pg_cap * sizeof *g_pg_pass); }
    pg_pass *p = &g_pg_pass[g_pg_np++]; memset(p, 0, sizeof *p);
    p->frame = f; p->pass = g_stat_pass; p->idx = g_pg_idx++; p->kind = kind; p->slot = (int)g_pg_used; p->draws = -(int)g_stat_draw;
    g_pg_used += nslot;
    return p;
}
static void pg_render(MTLRenderPassDescriptor *pd, NSUInteger nc) {
    (void)pd; (void)nc;
    return;
#if 0
    pg_pass *p = pg_new(0, 4); if (!p) return;
    MTLRenderPassSampleBufferAttachmentDescriptor *a = pd.sampleBufferAttachments[0];
    a.sampleBuffer = g_pg_buf[g_pg_b];
    a.startOfVertexSampleIndex = p->slot; a.endOfVertexSampleIndex = p->slot + 1; a.startOfFragmentSampleIndex = p->slot + 2; a.endOfFragmentSampleIndex = p->slot + 3;
    id<MTLTexture> t0 = nil; int k = 0;
    for (NSUInteger i = 0; i < nc && i < 8; i++) { id<MTLTexture> t = pd.colorAttachments[i].texture; if (!t) continue;
        if (!t0) t0 = t; if (k < 10) p->load[k++] = pd.colorAttachments[i].loadAction == MTLLoadActionClear ? 'C' : 'L'; }
    id<MTLTexture> dt = pd.depthAttachment.texture;
    if (dt) { if (k < 10) { p->load[k++] = '|'; p->load[k++] = pd.depthAttachment.loadAction == MTLLoadActionClear ? 'C' : 'L'; } p->dfmt = (uint32_t)dt.pixelFormat; }
    if (!t0) t0 = dt;
    NSUInteger pw, ph; pass_dims(pd, nc, &pw, &ph);
    p->w = (int)pw; p->h = (int)ph; p->cfmt0 = t0 && t0 != dt ? (uint32_t)t0.pixelFormat : 0;
    g_pg_open = g_pg_np - 1;
#endif
}
static id<MTLComputeCommandEncoder> pg_compute(void) {
    pg_pass *p = pg_new(1, 2);
    ser_gate();
    if (!p) { args_dirty_all(); return enc_c_begin(NULL); }
    MTLComputePassDescriptor *cd = [MTLComputePassDescriptor computePassDescriptor];
    cd.sampleBufferAttachments[0].sampleBuffer = g_pg_buf[g_pg_b];
    cd.sampleBufferAttachments[0].startOfEncoderSampleIndex = p->slot; cd.sampleBufferAttachments[0].endOfEncoderSampleIndex = p->slot + 1;
    p->draws = 0;
    args_dirty_all();
    return enc_c_begin(cd);
}
static void pg_end_render(void) { if (g_pg_open >= 0) { g_pg_pass[g_pg_open].draws += (int)g_stat_draw; g_pg_open = -1; } }
static void pg_commit(id<MTLCommandBuffer> cb) {   // Hand this submission's pass table to the completion callback for resolving
    if (!g_pg || !g_pg_np) return;
    int n = g_pg_np; pg_pass *ps = malloc(n * sizeof *ps); memcpy(ps, g_pg_pass, n * sizeof *ps);
    id<MTLCounterSampleBuffer> buf = g_pg_buf[g_pg_b]; uint32_t used = g_pg_used;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        (void)c;
        NSData *d = [buf resolveCounterRange:NSMakeRange(0, used)];
        const MTLCounterResultTimestamp *ts = d.bytes;
        pthread_mutex_lock(&g_pg_mu);
        for (int i = 0; d && i < n; i++) {
            pg_pass *p = &ps[i]; uint64_t a, b;
            #define OK(x) ((x) != MTLCounterErrorValue && (x) != 0)
            if (p->kind == 0) { a = OK(ts[p->slot].timestamp) ? ts[p->slot].timestamp : ts[p->slot + 2].timestamp; b = OK(ts[p->slot + 3].timestamp) ? ts[p->slot + 3].timestamp : ts[p->slot + 1].timestamp; }
            else { a = ts[p->slot].timestamp; b = ts[p->slot + 1].timestamp; }
            if (!OK(a) || !OK(b) || b < a) continue;
            #undef OK
            static uint64_t prev_end;   // Latest end time of the passes completed so far (completion callbacks are serialized in submission order)
            double ms = (b - a) / 1e6;   // Timestamps are in ns, same as the CPU (measured 1:1 via sampleTimestamps on this machine)
            double inc = b > prev_end ? (b - (a > prev_end ? a : prev_end)) / 1e6 : 0;
            if (b > prev_end) prev_end = b;
            fprintf(g_pg_f, "%llu\t%d\t%c\t%.4f\t%.4f\t%d\t%d\t%u\t%u\t%s\t%d\t%llu\t%llu\t%llu\n", (unsigned long long)p->frame, p->idx, p->kind ? 'C' : 'R', ms, inc,
                    p->w, p->h, p->cfmt0, p->dfmt, p->load, p->draws, (unsigned long long)a, (unsigned long long)b, (unsigned long long)p->pass);
            if (p->idx < 256 && p->frame > 300) { pg_stat *st = &g_pg_st[p->idx]; st->sum += inc; st->n++; st->last = inc; st->meta = *p; }
            if (p->frame >= g_pg_rep_frame + 300 && p->frame > 300) {   // Report the top five every 300 frames (accumulated from frame 300)
                g_pg_rep_frame = p->frame; double tot = 0; int top[5] = {-1, -1, -1, -1, -1};
                for (int k = 0; k < 256; k++) if (g_pg_st[k].n) { double m = g_pg_st[k].sum / g_pg_st[k].n; tot += m;
                    for (int j = 0; j < 5; j++) if (top[j] < 0 || m > g_pg_st[top[j]].sum / g_pg_st[top[j]].n) { for (int q = 4; q > j; q--) top[q] = top[q - 1]; top[j] = k; break; } }
                char line[1024]; size_t o = 0;
                for (int j = 0; j < 5 && top[j] >= 0; j++) { pg_stat *t = &g_pg_st[top[j]];
                    o += snprintf(line + o, sizeof line - o, " #%d %.2f ms %dx%d fmt%u d%u %s %d 绘制；", top[j], t->sum / t->n, t->meta.w, t->meta.h, t->meta.cfmt0, t->meta.dfmt, t->meta.load, t->meta.draws); }
                MLOG("[passgpu] 至帧 %llu 每帧通道均值合计 %.2f ms；前五：%s", (unsigned long long)p->frame, tot, line);
                fflush(g_pg_f);
            }
        }
        pthread_mutex_unlock(&g_pg_mu);
        free(ps);
    }];
}
void ts_after(void);
void ms_after(void);
// ---------- Store lookahead: at pass end, decide from the later commands of the same submission whether each attachment's write-back is redundant ----------
// On TBDR every attachment is written back to memory in full when the pass ends; Switch (Maxwell immediate-mode rendering) has no such cost. If an attachment is cleared in full before being read, the write-back is redundant (DontCare).
// For the decision see mtl_sub_index / sdx_decide; only what can be proven within the same submission is accepted, and anything still undecided at the end of the submission is always written back.
typedef struct { uint64_t ser, bytes; uint32_t lv, sl; int idx; uint8_t depth, state, ridx, drawn, why, load, wrote, wwhy; const char *wkey; int wslot; } sd_att;   // depth: 0 color 1 depth 2 stencil (the two parts of a combined depth-stencil texture are decided separately)   // state 0 undecided 1 needed 2 redundant; idx: color attachment index, -1 for depth
static sd_att g_sd[9]; static int g_nsd;
static int g_udc_ab = -1, g_udc_ab_on;
// Attachments this pass did not write (depth / stencil test only, color write mask 0, etc.) get their write-back changed to DontCare: the content is already in memory, saving one full write-back.
// On by default: on Mac, same-process on / off screenshots follow the same distribution as the null control; on A19 Pro, private Depth32F_S8 / RGBA8 read back after DontCare differ in 0 places.
// DM_NVN_UNWRITTEN_DC=0 turns it off (for equivalence comparison)
static int udc_on(void) {
    if (g_udc_ab < 0) { const char *e = getenv("DM_NVN_UNWRITTEN_DC"); g_udc_ab = !(e && !strcmp(e, "0"));
        const char *a = getenv("DM_NVN_UDC_AB"); if (a && !strcmp(a, "1")) { g_udc_ab = 0; g_udc_ab_on = 0; } }
    return g_udc_ab || g_udc_ab_on;
}
// Diagnostic DM_NVN_TBDR=0: turns off all TBDR optimizations (store lookahead, load elision, same-target continuation, clear-only pass hoist, on-demand depth-stencil attach); replay takes the plainest 'attach all, load all, store all' path.
// Only for equivalence comparison (the same replay run on / off, comparing the picture byte by byte), not a product configuration
// Mario: off by default (only DM_NVN_TBDR=1 turns it on). These optimizations infer read / write relations from "which textures are bound in texture slots", while 38% of this game's shaders use bindless textures (handles in uniform buffers, NVN_RB_TEXU),
//   so the inference would miss readers - e.g. deciding an attachment is never read afterwards and not storing it. Turn them back on once they recognize bindless readers.
static int tbdr_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_TBDR"), *r = getenv("DM_NVN_RULER"); v = e && !strcmp(e, "1"); g_ruler_truth = r && !strcmp(r, "1"); if (!v) MLOG("TBDR 优化缺省关（本作有无绑定贴图；DM_NVN_TBDR=1 开）"); } return v && !g_ruler_truth; }
int dm_nvn_ruler_truth(void) { tbdr_on(); return g_ruler_truth; }   // Visual ruler ground-truth phase (host-side settings under test yield accordingly, e.g. the 3D render scale)
static int rt_range_on(void) { static int v = -1; if (v < 0) { const char *e=getenv("DM_NVN_RT_RANGE"); v=e && !strcmp(e,"1"); } return v; }   // Narrow candidate, off by default
static uint64_t g_sd_dead_bytes, g_sd_n_dead, g_sd_n_all;
// ---------- Cross-submission attachment validity ledger ----------
// Entry = subresource (texture serial, level, layer) + state, surviving across submissions:
//   XV_INVALID       content was not written back / not generated and must not be read. The next touch must be a full Clear, or use as an attachment with load action DontCare / Clear; otherwise it is a violation.
//   XV_PENDING_CLEAR reserved for 'clear-only pending across submissions' (not implemented): accepts only full, unscissored clears with the write mask fully open; clearing again while pending = replace the clear value without landing it;
//                    as an attachment with load DontCare / Clear → the pending clear is voided, with load Load → changed to a Clear load; depth-stencil is recorded per plane.
// Violation = report a failure + that stable key (format / width / height / level) permanently disabled (in memory + persisted to DM_XV_FILE, read at the next launch) + all entries of the same texture deleted. The fallback can only detect, not undo (in that frame the attachment holds old content).
// Its first use: color attachments 'scanned to the end undecided' at the end of a submission are not written back (sd_finish) - conditions: its first touch in this submission is a full clear / DontCare load (reset every frame before use), it is not a window texture, and the key is not disabled.
// Switch DM_NVN_XV=0 turns it off; has no effect when tbdr_on()==0 (visual ruler ground-truth mode).
// Trial period (XV_PROBE): for a key's first XV_TRIAL times it is only 'shadow-accounted' - written back as usual, but its next touch is tracked all the same; only after XV_TRIAL consecutive compliant touches does the write-back really start being skipped.
//   On a read / Load during the trial period: only disable that key (within this process, not persisted, no failure), the picture is intact - in practice there are readers as early as the launch / menu screens (upscale target 36), and not every fresh install should get one wrong frame.
enum { XV_INVALID = 1, XV_PENDING_CLEAR = 2, XV_PROBE = 3 };
#define XV_TRIAL 3
typedef struct { uint64_t ser, tag; uint32_t lv, sl; uint8_t state; } xv_ent;
static xv_ent g_xv[64]; static int g_xv_n, g_xv_block, g_xv_init;
static uint64_t g_xv_disabled[128]; static int g_xv_ndis;
static struct { uint64_t tag; int good; } g_xv_trial[128]; static int g_xv_ntrial;   // Number of consecutive compliant touches for each key
static int xv_trial_get(uint64_t tag, int add) { for (int i = 0; i < g_xv_ntrial; i++) if (g_xv_trial[i].tag == tag) return g_xv_trial[i].good += add; if (g_xv_ntrial < 128) { g_xv_trial[g_xv_ntrial].tag = tag; return g_xv_trial[g_xv_ntrial++].good = add; } return 0; }
static uint64_t g_xv_present[128]; static int g_xv_npresent;
static uint64_t g_xv_dc, g_xv_viol;
typedef struct { uint64_t ser; uint32_t lv, sl; uint8_t good; } xv_touch;
static xv_touch g_xv_touch[128]; static int g_xv_ntouch;
static uint64_t xv_tag(const tex_t *t, uint32_t lv) { return ((uint64_t)t->d.format << 40) ^ ((uint64_t)t->d.width << 24) ^ ((uint64_t)t->d.height << 8) ^ lv; }
// Stable key (unchanged across launches, does not use the texture serial): format / width / height / level, then mixed with the formats of the color attachments of the pass where the write-back is skipped and this attachment's index - textures of the same format and size but different purpose (e.g. transmittance 27 and upscale target 36) do not share a key
static uint64_t xv_tag_pass(const tex_t *t, uint32_t lv, int idx, const p_set_rt *rt) { uint64_t h = xv_tag(t, lv) ^ ((uint64_t)(idx + 1) << 60); for (int i = 0; i < rt->n && i < 8; i++) h = (h ^ (rt->color[i] ? (uint64_t)rt->color[i]->d.format + 1 : 0)) * 0x100000001b3ull; return h; }
static void xv_init(void) {
    if (g_xv_init) return; g_xv_init = 1;
    const char *p = getenv("DM_XV_FILE"); if (!p || !*p) return;
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[256], why[160]; unsigned long long tag; int ch, n = 0;
    while (fgets(line, sizeof line, f) && g_xv_ndis < 128) if (sscanf(line, "%llx\t%159[^\t]\t%d", &tag, why, &ch) >= 1) g_xv_disabled[g_xv_ndis++] = tag, n++;
    fclose(f); MLOG("读到 %d 条禁用记录", n);
}
static int xv_disabled(uint64_t tag) { xv_init(); for (int i = 0; i < g_xv_ndis; i++) if (g_xv_disabled[i] == tag) return 1; return 0; }
static xv_ent *xv_find(uint64_t ser, uint32_t lv, uint32_t sl) { for (int i = 0; i < g_xv_n; i++) if (g_xv[i].ser == ser && g_xv[i].lv == lv && g_xv[i].sl == sl) return &g_xv[i]; return NULL; }
static void xv_drop(uint64_t ser, uint32_t lv, uint32_t sl) { for (int i = 0; i < g_xv_n; i++) if (g_xv[i].ser == ser && g_xv[i].lv == lv && g_xv[i].sl == sl) { g_xv[i] = g_xv[--g_xv_n]; return; } }
static void xv_drop_all_of(uint64_t ser) { for (int i = 0; i < g_xv_n;) if (g_xv[i].ser == ser) g_xv[i] = g_xv[--g_xv_n]; else i++; }
static void xv_set(uint64_t ser, uint32_t lv, uint32_t sl, uint8_t state, uint64_t tag) {
    xv_init(); if (g_xv_block || xv_disabled(tag)) return;
    xv_ent *e = xv_find(ser, lv, sl); if (e) { e->state = state; e->tag = tag; return; }
    if (g_xv_n < 64) g_xv[g_xv_n++] = (xv_ent){ser, tag, lv, sl, state};
}
static void xv_violation(const xv_ent *e, const char *why) {
    xv_init();
    if (e->state == XV_PROBE) {   // Trial period: the content is still there and the picture is intact - only disable the key (this process)
        if (!xv_disabled(e->tag) && g_xv_ndis < 128) g_xv_disabled[g_xv_ndis++] = e->tag;
        PD("V xv 试用期见读者：键=%016llx 贴图=%llu 级=%u 原因=%s → 该键不做", (unsigned long long)e->tag, (unsigned long long)e->ser, e->lv, why);
        xv_drop_all_of(e->ser); return; }
    g_xv_viol++;
    int seen = xv_disabled(e->tag); if (!seen && g_xv_ndis < 128) g_xv_disabled[g_xv_ndis++] = e->tag;
    MLOG("【红】xv 标签=%016llx 贴图=%llu 级=%u 层=%u 原因=%s 通道=%llu", (unsigned long long)e->tag, (unsigned long long)e->ser, e->lv, e->sl, why, (unsigned long long)g_stat_pass);
    const char *p = getenv("DM_XV_FILE"); if (p && *p) { FILE *f = fopen(p, "a"); if (f) { fprintf(f, "%016llx\t%s\t%llu\n", (unsigned long long)e->tag, why, (unsigned long long)g_stat_pass); fclose(f); } }
    xv_drop_all_of(e->ser);
}
static void xv_check(uint64_t ser, uint32_t lv, uint32_t sl, const char *why) { xv_ent *e = xv_find(ser, lv, sl); if (e && (e->state == XV_INVALID || e->state == XV_PROBE)) { xv_ent copy = *e; xv_violation(&copy, why); } }
static void xv_good(xv_ent *e) { if (e->state == XV_PROBE) xv_trial_get(e->tag, 1); xv_drop(e->ser, e->lv, e->sl); }   // Compliant touch: counts once toward the trial period, and the entry returns to valid
static void xv_touch_add(uint64_t ser, uint32_t lv, uint32_t sl, int good) { for (int i = 0; i < g_xv_ntouch; i++) if (g_xv_touch[i].ser == ser && g_xv_touch[i].lv == lv && g_xv_touch[i].sl == sl) return; if (g_xv_ntouch < 128) g_xv_touch[g_xv_ntouch++] = (xv_touch){ser, lv, sl, (uint8_t)good}; }
static int xv_first_good(uint64_t ser, uint32_t lv, uint32_t sl) { for (int i = 0; i < g_xv_ntouch; i++) if (g_xv_touch[i].ser == ser && g_xv_touch[i].lv == lv && g_xv_touch[i].sl == sl) return g_xv_touch[i].good; return 0; }
static int xv_present(uint64_t ser) { for (int i = 0; i < g_xv_npresent; i++) if (g_xv_present[i] == ser) return 1; return 0; }
static void xv_forward(void);
static int tbdr_on(void);
static int xv_on(void) { static int off = -1; if (off < 0) { const char *e = getenv("DM_NVN_XV"); off = e && !strcmp(e, "0"); } return !off && tbdr_on(); }
void mtl_xv_window_tex(uint64_t serial) { if (!xv_present(serial) && g_xv_npresent < 128) g_xv_present[g_xv_npresent++] = serial; }   // Window (swapchain) texture: never enters the ledger
void xv_test_counts(int *dc, int *viol) { if (dc) *dc = (int)g_xv_dc; if (viol) *viol = (int)g_xv_viol; }
static void sd_finish(void);
// Load elision (fully covering first draw): if the draw that opens the pass can be proven to cover a color attachment in full (first_draw_covers), that attachment's load becomes DontCare -
// the first draw does not blend and writes all channels, every covered pixel is rewritten by it, and the original content is not visible (Switch immediate-mode rendering has no such cost; on TBDR load = reading the whole thing from memory into the tile)
static const p_draw *g_fd_dw; static const p_draw_idx *g_fd_di;   // The draw that opens the pass (one of non-indexed / indexed); non-null only while draw() is opening a pass
static uint64_t g_fd_n;         // Cumulative number of attachments judged elidable
static int g_fd_why;            // Reason for the most recent failure (Q line of PASSDUMP: 1 no program 2 shader 3 attachment 4 blend/mask/raster 5 depth-stencil test 6 vertex count 7 position attribute 8 format/stream 9 out of bounds 10 address 11 geometry)
static int first_draw_covers(int i, id<MTLTexture> t, NSUInteger level);
static int pf_is_float(MTLPixelFormat f) {   // Float color formats that can store NaN (sentinel choice for the load-elision probe; RGB9E5 cannot store NaN and does not count)
    switch (f) { case MTLPixelFormatR16Float: case MTLPixelFormatRG16Float: case MTLPixelFormatRGBA16Float: case MTLPixelFormatR32Float:
        case MTLPixelFormatRG32Float: case MTLPixelFormatRGBA32Float: case MTLPixelFormatRG11B10Float: return 1; default: return 0; }
}
static int fz_load_covers(int i, id<MTLTexture> t, NSUInteger level);
static NSMutableArray *g_lp;    // DM_NVN_LOADPROBE=1 / 2 / 3 (two constant sentinel values / NaN - under float formats normal shader output cannot reach NaN, ruling out numeric coincidence): attachments judged elidable are cleared to the sentinel color instead, and at pass end the remaining sentinel pixels are read back and counted (diagnostic: should be 0)
static void lp_finish(void);
// Same-target continuation: when the game calls SetRenderTargets again on the same set of targets (with no copy / dispatch / clear in between) the pass is not ended - on TBDR this saves one full write-back + load.
// Basis for equivalence: NVN's Barrier between two passes requires the latter to see all writes of the former. Within one Metal render encoder, later draws' reads and writes of the attachment itself (blend, depth / stencil test)
// are ordered with earlier draws in submission order; but 'sampling' the attachment texture is not covered (reading and writing the same attachment within one pass is undefined). So continuation happens only when later draws do not sample these attachments:
// if during continuation a draw needs to sample them, or a clear arrives, the pass is ended before it, returning to the separate semantics. Copies / dispatches end the render encoder anyway
static int g_enc_mode;          // How the current encoder was opened (rp_begin_ex argument: 0 all attachments 1 only for clears 2 depth omitted)
static int g_rt_range_active, g_rt_range_pending;
static int g_rt_merged;         // The current pass has already been continued (at least one same-target SetRenderTargets was merged in)
static uint64_t g_rt_merge_n;   // Cumulative continuation count
static int g_defer_replaying;
static void fz_end(void);   // Wrap-up of the readable-tile pass merge (defined before draw)
static void defer_fallback(void);
// DM_NVN_ENC_LABEL=1: label every render encoder with 'p<index of the render pass within the frame> <width>x<height> <first 8 digits of the first draw's fragment key> d<draw count>'; in Instruments (xctrace Game Performance / Metal System Trace)
// the encoder track and the GPU interval table carry this name directly, and shader_map.tsv maps it back to the shader; off by default (one string allocation per pass)
static int g_el_w, g_el_h, g_el_draws; static char g_el_key[12];
static int enc_label_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_ENC_LABEL"); v = e ? atoi(e) : 1; } return v; }
static void ghost_flush(void);
static void za_atomic_sync_end(void);
static void zero_image_sync_end(void);
static void za_diag_pass_end(void);
static void za_diag_op(const char *kind, const tex_t *t);
static void pixel_history_begin(MTLRenderPassDescriptor *pd, NSUInteger nc);
static void pixel_history_store(void);
static void pixel_history_end(void);
static void pixel_history_draw(const char *vs, const char *fs);
static void rp_end(void) { if (g_enc) { AP_B;
    ghost_flush();
    if (enc_label_on()) { static uint64_t lf = ~0ull, base; uint64_t f = atomic_load(&g_cur_frame); if (f != lf) { lf = f; base = g_stat_pass - 1; }   // Intra-frame serial: the same pass has the same name every frame, so the table can aggregate by name
        [(id)g_enc setLabel:[NSString stringWithFormat:@"p%02llu %dx%d %s d%d", (unsigned long long)(g_stat_pass - base), g_el_w, g_el_h, g_el_key[0] ? g_el_key : "-", g_el_draws]]; }
#ifdef NVN_RT_RANGE_TEST
    int ranged=g_rt_range_active && !g_rt_range_pending;
#endif
    sd_finish(); pixel_history_store(); enc_r_end_pass(g_enc); g_enc = nil; zero_image_sync_end(); za_atomic_sync_end(); za_diag_pass_end(); pixel_history_end(); zskip_commit_clears(); g_rt_merged = 0; g_rt_range_active = g_rt_range_pending = 0; pg_end_render(); ts_after(); ms_after(); if (g_lp.count) lp_finish(); fz_end();
    if (g_nubd) ubd_flush();   // the inline updates deferred in this pass, in order (see mtl_sub_update)
#ifdef NVN_RT_RANGE_TEST
    rr_finish(ranged);
#endif
    AP_E(0);
} else zero_image_sync_end(); }
void pg_sub_begin_ext(uint64_t fence) { pg_sub_begin(fence); }
void pg_commit_ext(id<MTLCommandBuffer> cb) { pg_commit(cb); }
// The format an attachment is actually attached with: the view's format if the view changes the format and can be attached, otherwise the base texture's. Shared by attaching (attach) and pipeline prediction (pso_predict_rest) so both agree --
// prediction used the base format, so for a pass written through an sRGB view (Mario's last pass) the predicted pipeline had the wrong format and the real one was compiled at the first draw (from d2r-mobile e2a0c9a9b)
// A compressed texture as a render target (The Legend of Zelda: Breath of the Wild: a 32x32 RGTC1 (BC4) texture written through an RGBA16UI view, one texel = one 8-byte block, i.e. BC4 encoded on the GPU):
//   Metal can neither attach a compressed format as a colour attachment nor create compressed <-> uncompressed views (the pipeline descriptor validation aborts the process). For now the attachment is left out (a pass with no attachment at all skips its draws), with a red line;
//   drawing it correctly needs an uncompressed proxy texture with the same block size, copied into the compressed texture through a buffer after the pass
//   On a GPU without BC the same texture is stored decompressed (R8 for BC4), so the attachment check must go by the NVN format, not the Metal one: attaching it through the RGBA16UI view
//   asked Metal for an R8Unorm -> RGBA16Uint view, which aborts the process (GitHub issue #4, iPhone 14 Pro; DM_TEST_NO_BC=1 reproduces it on the Mac)
static int mtl_fmt_block(MTLPixelFormat f) { return (f >= (MTLPixelFormat)DM_MTL_BC1 && f <= (MTLPixelFormat)DM_MTL_BC7_SRGB) || (f >= MTLPixelFormatPVRTC_RGB_2BPP && f < MTLPixelFormatGBGR422); }
static int nvn_fmt_bc(uint32_t f) { uint32_t pf; int bpp; return nvn_bc_unpacked(f, &pf, &bpp); }   // NVN BC1-7 / BC6H formats, whether or not this GPU stores them compressed
static MTLPixelFormat rt_view_fmt(id<MTLTexture> base, const view_t *v, int has_v) {
    if (base && mtl_fmt_block(base.pixelFormat)) {
        RED_ONCE(78, "渲染目标是压缩格式贴图（Metal 格式 %lu %lux%lu 层 %lu，视图换格式 %d → NVN %#x）：不挂，该附件的绘制落空", (unsigned long)base.pixelFormat, (unsigned long)base.width, (unsigned long)base.height,
                 (unsigned long)base.arrayLength, has_v ? v->set_format : 0, has_v ? v->format : 0);
        return MTLPixelFormatInvalid;
    }
    if (!base || !has_v || !v->set_format) return base.pixelFormat;
    uint32_t pf = 0; uint8_t swz[4];
    if (!nvn_fmt_to_mtl(v->format, &pf, swz) || !pf || pf == (uint32_t)base.pixelFormat) return base.pixelFormat;
    if (tex_need_pfv(base.pixelFormat, (MTLPixelFormat)pf) && !(base.usage & MTLTextureUsagePixelFormatView)) return base.pixelFormat;
    return (MTLPixelFormat)pf;
}
// The same texture level / layer attached at more than one colour slot: only the first is attached, later ones are not (their writes are dropped). The G-buffer passes of The Legend of Zelda: Breath of the Wild attach the albedo and another buffer twice each
// (c1 = c2, c3 = c4), and the channel write masks of c2 / c4 are always 0 (cmask 0x00f0f0ff). The Switch's ROP writes only the masked channels and the two slots do not interfere;
// on Apple's TBDR each attachment has its own tile memory and the whole tile is stored back at the end of the pass, so the copy stored later (loaded, never written) overwrites the writes of the earlier one -- the order differs per 32x32 tile,
// the albedo kept only scattered squares, and the whole 3D picture went black. A red line is printed when a dropped slot is used with a non-zero write mask (those writes are really lost)
static int rt_dup_slot(const p_set_rt *r, int i) {
    if (!r->color[i]) return 0;
    uint8_t lv = r->has_cview[i] && r->cview[i].set_levels ? r->cview[i].base_level : 0; uint16_t ly = r->has_cview[i] && r->cview[i].set_layers ? r->cview[i].min_layer : 0;
    for (int j = 0; j < i; j++) {
        if (!r->color[j] || r->color[j]->serial != r->color[i]->serial) continue;   // compare serials: records hold copies of the texture objects (tex_snap), so the pointers differ
        uint8_t lj = r->has_cview[j] && r->cview[j].set_levels ? r->cview[j].base_level : 0; uint16_t yj = r->has_cview[j] && r->cview[j].set_layers ? r->cview[j].min_layer : 0;
        if (lj == lv && yj == ly) return 1;
    }
    return 0;
}
static void attach(MTLRenderPassAttachmentDescriptor *a, const tex_t *t, const view_t *v, int has_v) {
    za_diag_op("render-target", t);
    id<MTLTexture> base = tex_mtl(t); a.texture = base;
    if (base && mtl_fmt_block(base.pixelFormat)) { rt_view_fmt(base, v, has_v); a.texture = nil; return; }   // compressed formats cannot be attached (see rt_view_fmt)
    if (base && nvn_fmt_bc(t->d.format)) {   // the same texture stored decompressed on a GPU without BC: not attached either (see above rt_view_fmt)
        RED_ONCE(78, "渲染目标是压缩格式贴图（无 BC，按解压格式存：Metal 格式 %lu %lux%lu，视图换格式 %d → NVN %#x）：不挂，该附件的绘制落空", (unsigned long)base.pixelFormat, (unsigned long)base.width, (unsigned long)base.height,
                 has_v ? v->set_format : 0, has_v ? v->format : 0);
        a.texture = nil; return;
    }
    if (has_v && v->set_levels) a.level = v->base_level;
    if (has_v && v->set_layers) { if (t->d.target == 2) a.depthPlane = v->min_layer; else a.slice = v->min_layer; }
    // A view that changes the format is attached as a Metal view of that format (pipeline formats follow the attachment). SMBW's last pass writes the RGBA8 swapchain texture through an RGBA8_SRGB view, so the hardware encodes sRGB on write;
    // attaching the base texture instead stored the linear values as they were and the whole picture came out dark (every value decoded from sRGB once more, on the Mac and the iPhone). The view lives until this submission completes
    if (base && has_v && v->set_format) {
        MTLPixelFormat pf = rt_view_fmt(base, v, has_v);
        if (pf != base.pixelFormat) {
            id<MTLTexture> vw = [base newTextureViewWithPixelFormat:pf];
            if (vw) { a.texture = vw; defer_obj_unref(vw); }
            else RED_ONCE(67, "渲染目标视图建不出：格式 %#x（底图 Metal 格式 %lu）", v->format, (unsigned long)base.pixelFormat);
        } else { uint32_t want = 0; uint8_t swz[4];
            if (nvn_fmt_to_mtl(v->format, &want, swz) && want && want != (uint32_t)base.pixelFormat)
                RED_ONCE(68, "渲染目标视图格式 %#x 挂不上（底图 Metal 格式 %lu 建时没有换格式用途）：按底图格式画", v->format, (unsigned long)base.pixelFormat); }
    }
}

// Metal requires renderTargetArrayLength for layered rendering. Without it,
// a vertex shader's [[render_target_array_index]] is effectively confined to
// layer zero on iOS 16. Z-A exposes this with its instanced 32x32x32 colour-LUT
// pass: all 32 instances run, but only depth plane zero is written and the
// following tone mapper samples zero from the other 31 planes.
//
// A 3D attachment without a single-layer view represents the remaining depth
// planes starting at depthPlane. Multiple attachments must expose the same
// number of active layers, so return one (the non-layered default) if any
// attached target cannot cover the candidate range.
static NSUInteger pass_3d_layer_count(MTLRenderPassDescriptor *pd, NSUInteger nc) {
    NSUInteger layers = NSUIntegerMax;
    int found_3d = 0;
    for (NSUInteger i = 0; i < nc; i++) {
        MTLRenderPassColorAttachmentDescriptor *a = pd.colorAttachments[i];
        id<MTLTexture> t = a.texture;
        if (!t) continue;
        NSUInteger n = 1;
        if (t.textureType == MTLTextureType3D) {
            NSUInteger depth = MAX(t.depth >> a.level, 1ul);
            n = a.depthPlane < depth ? depth - a.depthPlane : 1;
            if (i < (NSUInteger)R.rt.n && R.rt.has_cview[i] && R.rt.cview[i].set_layers && R.rt.cview[i].nlayers)
                n = MIN(n, (NSUInteger)R.rt.cview[i].nlayers);
            found_3d |= n > 1;
        }
        layers = MIN(layers, n);
    }
    // Depth/stencil attachments participate in every active layer too. A
    // conventional 2D depth target therefore makes this a one-layer pass.
    MTLRenderPassAttachmentDescriptor *extra[2] = { pd.depthAttachment, pd.stencilAttachment };
    for (int i = 0; i < 2; i++) if (extra[i].texture) {
        id<MTLTexture> t = extra[i].texture;
        NSUInteger n = 1;
        if (t.textureType == MTLTextureType3D) {
            NSUInteger depth = MAX(t.depth >> extra[i].level, 1ul);
            n = extra[i].depthPlane < depth ? depth - extra[i].depthPlane : 1;
            found_3d |= n > 1;
        }
        layers = MIN(layers, n);
    }
    return found_3d && layers != NSUIntegerMax ? layers : 1;
}
// Initial fallback supports color-only, single-sample 3D attachments with an
// identical relative layer range. Other attachment combinations stay explicit.
static NSUInteger slice_target_count(void) {
    if (g_layered_native || !R.has_rt || R.rt.depth) return 1;
    NSUInteger count = 0;
    for (int i = 0; i < R.rt.n && i < 8; i++) if (R.rt.color[i]) {
        const tex_t *t = R.rt.color[i]; const view_t *v = &R.rt.cview[i]; int hv = R.rt.has_cview[i];
        if (t->d.target != 2 || t->d.samples > 1) return 1;
        NSUInteger level = hv && v->set_levels ? v->base_level : 0;
        NSUInteger base = hv && v->set_layers ? v->min_layer : 0;
        NSUInteger depth = MAX((NSUInteger)t->d.depth >> level, 1ul);
        if (base >= depth) return 1;
        NSUInteger n = depth - base;
        if (hv && v->set_layers && v->nlayers) n = MIN(n, (NSUInteger)v->nlayers);
        if (count && n != count) return 1;
        count = n;
    }
    return count ? count : 1;
}
static void slice_select(uint32_t slice) {
    for (int i = 0; i < R.rt.n && i < 8; i++) if (R.rt.color[i]) {
        view_t *v = &R.rt.cview[i];
        v->min_layer = (R.rt.has_cview[i] && v->set_layers ? v->min_layer : 0) + slice;
        v->set_layers = 1; v->nlayers = 1; R.rt.has_cview[i] = 1;
    }
    g_selected_slice = slice;
}
static void bare_dims(NSUInteger *w, NSUInteger *h);
static int diag_fill_c0_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("DM_NVN_FILL_C0");
        on = e && !strcmp(e, "1");
        if (on) MLOG("DM_NVN_FILL_C0=1: attach a transient color 0 target to sparse MRT passes");
    }
    return on;
}
static id<MTLTexture> diag_c0_texture(NSUInteger w, NSUInteger h, NSUInteger samples) {
    if (!g_diag_c0_cache) g_diag_c0_cache = [NSMutableDictionary dictionary];
    NSString *key = [NSString stringWithFormat:@"%lux%lu@%lu", (unsigned long)w, (unsigned long)h, (unsigned long)samples];
    id<MTLTexture> t = g_diag_c0_cache[key];
    if (t) return t;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
    d.pixelFormat = MTLPixelFormatRGBA16Float;
    d.width = MAX(w, 1ul); d.height = MAX(h, 1ul); d.sampleCount = MAX(samples, 1ul);
    d.storageMode = MTLStorageModeMemoryless; d.usage = MTLTextureUsageRenderTarget;
    t = [g_dev newTextureWithDescriptor:d];
    if (t) g_diag_c0_cache[key] = t;
    else MLOG("[RED] DM_NVN_FILL_C0 could not allocate %lux%lu@%lu", (unsigned long)w, (unsigned long)h, (unsigned long)samples);
    return t;
}
// mode 1 (clear_only): a pass opened only to land pending clears, attaching only the attachments that have clears; mode 2: the lookahead proves this pass does not use depth-stencil, so depth is not attached.
// clear_only: a pass opened only to land pending clears, attaching only the attachments that have clears - the other attachments are not touched by this pass, and attaching them costs a pointless load and store on TBDR
// (Mac character select screen: the color-clear pass drags along a 400x225 depth load and store)
static id<MTLRenderCommandEncoder> rp_begin_ex(int clear_only);
static id<MTLRenderCommandEncoder> rp_begin(void) { return rp_begin_ex(0); }
static id<MTLRenderCommandEncoder> rp_begin_ex_(int clear_only);
static id<MTLRenderCommandEncoder> rp_begin_ex(int clear_only) { AP_B; id<MTLRenderCommandEncoder> e = rp_begin_ex_(clear_only); AP_E(1); return e; }
static id<MTLRenderCommandEncoder> rp_begin_ex_(int clear_only) {
    if (g_enc) return g_enc;
    memset(g_zskip_cand_ok, 0, sizeof g_zskip_cand_ok);
    g_enc_mode = clear_only;
    if (!R.has_rt) { RED_ONCE(3, "绘制前没有设置渲染目标"); return nil; }
    // The version of the attachment-range candidate is taken before capturing any attachment; if it changes during setup, there is no continuation.
    uint64_t reg0 = 0, ser0 = 0, dirty0 = 0;
    if (rt_range_on()) { reg0 = g_reg_gen; ser0 = g_ser_gen; }
    if (rt_range_on()) dirty0=nvn_dirty_now();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor new];
    int any = 0, diag_c0 = 0;
    for (int i = 0; i < R.rt.n; i++) {
        if (!R.rt.color[i] || (clear_only == 1 && !R.clear_c[i])) continue;
        if (rt_dup_slot(&R.rt, i)) continue;   // a slot attached twice is not attached (see rt_dup_slot)
        attach(pd.colorAttachments[i], R.rt.color[i], &R.rt.cview[i], R.rt.has_cview[i]);
        if (!pd.colorAttachments[i].texture) continue;
        any = 1;
        pd.colorAttachments[i].loadAction = R.clear_c[i] ? MTLLoadActionClear : MTLLoadActionLoad;
        pd.colorAttachments[i].clearColor = R.cc[i];
        if (R.clear_c[i]) PD("C %llu c%d 清成 %g,%g,%g,%g", g_stat_pass + 1, i, R.cc[i].red, R.cc[i].green, R.cc[i].blue, R.cc[i].alpha);
        int cov = tbdr_on() && !R.clear_c[i] && (g_fd_dw || g_fd_di) && fz_load_covers(i, pd.colorAttachments[i].texture, pd.colorAttachments[i].level);
        if (!R.clear_c[i] && (g_fd_dw || g_fd_di) && !cov) PD("Q %llu c%d 读入不可省 why=%d", g_stat_pass + 1, i, g_fd_why);
        if (cov) {
            static int probe = -1; if (probe < 0) { probe = getenv("DM_NVN_LOADPROBE") ? atoi(getenv("DM_NVN_LOADPROBE")) : 0; if (probe) MLOG("DM_NVN_LOADPROBE：读入可省的附件改清成哨兵色并回读核对"); }
            if (probe) { pd.colorAttachments[i].loadAction = MTLLoadActionClear; int pm = probe == 1 && pf_is_float(pd.colorAttachments[i].texture.pixelFormat) ? 3 : probe;   // Float formats use a NaN sentinel by default: a fixed sentinel value can be hit by real output (it has been hit in practice)
                pd.colorAttachments[i].clearColor = pm == 3 ? MTLClearColorMake(NAN, NAN, NAN, NAN) : probe == 2 ? MTLClearColorMake(0.777, 0.222, 0.555, 0.888) : MTLClearColorMake(0.123, 0.456, 0.789, 0.321);
                if (!g_lp) g_lp = [NSMutableArray array];
                [g_lp addObject:@[pd.colorAttachments[i].texture, @(pd.colorAttachments[i].level), @(pd.colorAttachments[i].slice), @(g_stat_pass + 1), @(i), @(R.rt.color[i]->serial), @(pm)]]; }
            else pd.colorAttachments[i].loadAction = MTLLoadActionDontCare;
            g_fd_n++;
        }
        xv_touch_add(R.rt.color[i]->serial, (uint32_t)pd.colorAttachments[i].level, (uint32_t)pd.colorAttachments[i].slice,
                     pd.colorAttachments[i].loadAction != MTLLoadActionLoad);
        if (g_xv_n) { xv_ent *xe = xv_find(R.rt.color[i]->serial, (uint32_t)pd.colorAttachments[i].level, (uint32_t)pd.colorAttachments[i].slice);
            if (xe && (xe->state == XV_INVALID || xe->state == XV_PROBE)) { if (pd.colorAttachments[i].loadAction == MTLLoadActionLoad) { xv_ent copy = *xe; xv_violation(&copy, "作附件读入旧内容"); } else xv_good(xe); } }
        pd.colorAttachments[i].storeAction = MTLStoreActionStore;
    }
    if (!pd.colorAttachments[0].texture && clear_only != 1 && diag_fill_c0_on()) {
        MTLRenderPassColorAttachmentDescriptor *first = nil;
        for (int i = 1; i < R.rt.n && !first; i++) if (pd.colorAttachments[i].texture) first = pd.colorAttachments[i];
        if (first) {
            NSUInteger w = MAX(first.texture.width >> first.level, 1ul), h = MAX(first.texture.height >> first.level, 1ul);
            id<MTLTexture> t = diag_c0_texture(w, h, first.texture.sampleCount);
            if (t) {
                pd.colorAttachments[0].texture = t;
                pd.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                pd.colorAttachments[0].storeAction = MTLStoreActionDontCare;
                any = diag_c0 = 1;
                PD("P %llu c0 diagnostic filler fmt=%lu %lux%lu ms%lu", g_stat_pass + 1, (unsigned long)t.pixelFormat,
                   (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.sampleCount);
            }
        }
    }
    // clear_only: 0 attach all 1 only for clears 2 neither depth nor stencil attached 3 stencil only 4 depth only (3 / 4: the pass_ds_need lookahead proves the whole pass uses only that one)
    if (R.rt.depth && clear_only == 3) {   // Stencil only: the depth plane of the combined format does not enter the tile and is neither read nor written (Metal allows an empty depth attachment with the combined texture on the stencil attachment)
        attach(pd.stencilAttachment, R.rt.depth, &R.rt.dview, R.rt.has_dview);
        MTLPixelFormat pf = pd.stencilAttachment.texture.pixelFormat;
        if (pf == MTLPixelFormatDepth32Float_Stencil8 || pf == MTLPixelFormatStencil8) {
            any = 1; pd.stencilAttachment.loadAction = R.clear_s ? MTLLoadActionClear : MTLLoadActionLoad;
            pd.stencilAttachment.clearStencil = R.cs; pd.stencilAttachment.storeAction = MTLStoreActionStore;
        } else pd.stencilAttachment.texture = nil;   // No stencil plane: the stencil test always passes, attach nothing
    } else if (R.rt.depth && clear_only != 2 && !(clear_only == 1 && !R.clear_d && !R.clear_s)) {
        attach(pd.depthAttachment, R.rt.depth, &R.rt.dview, R.rt.has_dview);
        if (pd.depthAttachment.texture) {
            any = 1;
            pd.depthAttachment.loadAction = R.clear_d ? MTLLoadActionClear : MTLLoadActionLoad;
            pd.depthAttachment.clearDepth = R.cd; pd.depthAttachment.storeAction = MTLStoreActionStore;
            MTLPixelFormat pf = pd.depthAttachment.texture.pixelFormat;
            if (clear_only != 4 && (pf == MTLPixelFormatDepth32Float_Stencil8 || pf == MTLPixelFormatStencil8)) {   // 4: depth only
                pd.stencilAttachment.texture = pd.depthAttachment.texture; pd.stencilAttachment.level = pd.depthAttachment.level;
                pd.stencilAttachment.slice = pd.depthAttachment.slice;
                pd.stencilAttachment.loadAction = R.clear_s ? MTLLoadActionClear : MTLLoadActionLoad;
                pd.stencilAttachment.clearStencil = R.cs; pd.stencilAttachment.storeAction = MTLStoreActionStore;
            }
        }
    }
    // NVN permits a fragment shader whose only effects are image or storage
    // writes to draw with zero color/depth targets. Metal supports the same
    // operation when the render-pass size is explicit. Z-A uses this path for
    // its image-atomic pass; skipping it leaves the later 3D composite black.
    int bare = !any && clear_only != 1 && !R.rt.depth;
    for (int i = 0; i < R.rt.n && bare; i++) if (R.rt.color[i]) bare = 0;
    if (bare) {
        NSUInteger bw, bh;
        bare_dims(&bw, &bh);
        pd.renderTargetWidth = bw;
        pd.renderTargetHeight = bh;
        pd.defaultRasterSampleCount = 1;
        if (!g_bare_native) {
            // Apple3 requires a real attachment even when the shader writes
            // only buffers. Match the attachment-less raster bounds exactly;
            // no guest image is substituted and no tile content is stored.
            id<MTLTexture> t = diag_c0_texture(bw, bh, 1);
            if (!t) { MLOG("[RED] Could not allocate attachment-less compatibility target %lux%lu", (unsigned long)bw, (unsigned long)bh); return nil; }
            pd.colorAttachments[0].texture = t;
            pd.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            pd.colorAttachments[0].storeAction = MTLStoreActionDontCare;
            diag_c0 = 1;
        }
        static int once;
        if (!once++) MLOG("attachment-less pass: opened at viewport size %lux%lu (first case pass %llu)",
                          (unsigned long)bw, (unsigned long)bh, g_stat_pass + 1);
        PD("P %llu attachment-less %lux%lu", g_stat_pass + 1, (unsigned long)bw, (unsigned long)bh);
        any = 1;
    }
    if (!any) {
        RED_ONCE(4, "渲染目标都建不出 MTLTexture");
        if (passdump()) {   // PASSDUMP: when a whole pass is skipped, list its targets (serial, format, compressed or not)
            char b[256]; size_t o = 0;
            for (int i = 0; i < R.rt.n && i < 8; i++) if (R.rt.color[i]) o += snprintf(b + o, sizeof b - o, " c%d=%llu/f%#x%s", i, (unsigned long long)R.rt.color[i]->serial, R.rt.color[i]->d.format, fmt_compressed(R.rt.color[i]->d.format) ? "(压缩)" : "");
            if (R.rt.depth) o += snprintf(b + o, sizeof b - o, " d=%llu", (unsigned long long)R.rt.depth->serial);
            PD("N %llu 通道跳过（附件都挂不上）mode=%d%s", g_stat_pass + 1, clear_only, b);
        }
        return nil;
    }
    NSUInteger layer_count = pass_3d_layer_count(pd, (NSUInteger)R.rt.n);
    if (layer_count > 1) {
        if (!g_layered_native) {
            MLOG("[RED] Unsupported layered attachment combination: %lu layers; no proven per-slice replay", (unsigned long)layer_count);
            return nil;
        }
        pd.renderTargetArrayLength = layer_count;
        static int once;
        if (!once++) MLOG("layered 3D render target: renderTargetArrayLength=%lu (first pass %llu)",
                          (unsigned long)layer_count, (unsigned long long)(g_stat_pass + 1));
        PD("P %llu layered-3d layers=%lu", g_stat_pass + 1, (unsigned long)layer_count);
    }
    for (int i = 0; i < R.rt.n && i < 8; i++) if (R.rt.color[i] && zskip_zero_clear(i)) {
        g_zskip_cand_ok[i] = 1; g_zskip_cand[i] = (zskip_zero){R.rt.color[i]->serial, (uint16_t)(R.rt.has_cview[i] && R.rt.cview[i].set_levels ? R.rt.cview[i].base_level : 0), (uint16_t)(R.rt.has_cview[i] && R.rt.cview[i].set_layers ? R.rt.cview[i].min_layer : 0)};
    }
    // Bandwidth ledger: attachments with Load count as load, all attachments count as store (currently always Store)
    int nload = 0, natt = 0;
    for (int i = 0; i < R.rt.n; i++) {
        MTLRenderPassColorAttachmentDescriptor *c = pd.colorAttachments[i];
        if (!c.texture) continue;
        if (diag_c0 && i == 0) continue;
        uint64_t b = att_bytes(c.texture, c.level); natt++; g_bw_store += b; if (c.loadAction == MTLLoadActionLoad) { g_bw_load += b; nload++; }
        if (R.rt.has_cview[i] && R.rt.cview[i].set_format) PD("V %llu c%d 视图格式 %#x（基贴图 NVN 格式 %#x）", g_stat_pass + 1, i, R.rt.cview[i].format, R.rt.color[i]->d.format);
        PD("P %llu c%d ser=%llu fmt=%lu %lux%lu ms%lu lv%lu sl%lu %s mtl=%p", g_stat_pass + 1, i, R.rt.color[i]->serial, (unsigned long)c.texture.pixelFormat,
           (unsigned long)c.texture.width, (unsigned long)c.texture.height, (unsigned long)c.texture.sampleCount, (unsigned long)c.level, (unsigned long)c.slice,
           c.loadAction == MTLLoadActionClear ? "clear" : c.loadAction == MTLLoadActionDontCare ? "dontcare" : "load", (__bridge void *)(c.texture.parentTexture ? c.texture.parentTexture : c.texture));
    }
    {   // Depth and stencil planes are counted separately (combined format 5 bytes = depth 4 + stencil 1; when only one is attached only that one is counted)
        id<MTLTexture> dt = pd.depthAttachment.texture, stt = pd.stencilAttachment.texture;
        uint64_t whole = att_bytes(dt ? dt : stt, dt ? pd.depthAttachment.level : pd.stencilAttachment.level);
        int comb = (dt ? dt : stt).pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
        uint64_t db = dt ? (comb ? whole / 5 * 4 : whole) : 0, sb = stt && stt != dt ? (comb ? whole / 5 : whole) : stt ? whole - db : 0;
        if (dt) { natt++; g_bw_store += db; if (pd.depthAttachment.loadAction == MTLLoadActionLoad) { g_bw_load += db; nload++; } }
        if (stt) { natt += !dt; g_bw_store += sb; if (pd.stencilAttachment.loadAction == MTLLoadActionLoad) { g_bw_load += sb; nload += !dt; } }
        if (dt) PD("P %llu d ser=%llu fmt=%lu %lux%lu %s depth=%g", g_stat_pass + 1, R.rt.depth->serial, (unsigned long)dt.pixelFormat,
                   (unsigned long)dt.width, (unsigned long)dt.height, pd.depthAttachment.loadAction == MTLLoadActionClear ? "clear" : "load", pd.depthAttachment.clearDepth);
        else if (stt) PD("P %llu s ser=%llu fmt=%lu %lux%lu %s（只挂模板）", g_stat_pass + 1, R.rt.depth->serial, (unsigned long)stt.pixelFormat,
                   (unsigned long)stt.width, (unsigned long)stt.height, pd.stencilAttachment.loadAction == MTLLoadActionClear ? "clear" : "load");
    }
    if (natt && nload == natt) g_pass_full_rw++;
    g_nsd = 0;
    {
        for (int i = 0; i < R.rt.n && i < 8; i++) { MTLRenderPassColorAttachmentDescriptor *c = pd.colorAttachments[i]; if (!c.texture) continue;
            if (diag_c0 && i == 0) continue;
            g_sd[g_nsd] = (sd_att){0}; g_sd[g_nsd].ser = R.rt.color[i]->serial; g_sd[g_nsd].bytes = att_bytes(c.texture, c.level); g_sd[g_nsd].lv = (uint32_t)c.level; g_sd[g_nsd].sl = (uint32_t)(c.texture.textureType == MTLTextureType3D ? c.depthPlane : c.slice); g_sd[g_nsd].idx = i; g_sd[g_nsd].load = c.loadAction == MTLLoadActionLoad; g_sd[g_nsd].wrote = R.clear_c[i] != 0; g_nsd++;
            c.storeAction = MTLStoreActionUnknown; }
        id<MTLTexture> dt = pd.depthAttachment.texture, stt = pd.stencilAttachment.texture;
        uint64_t db = att_bytes(dt, pd.depthAttachment.level), sb = 0;
        if (stt) { int comb = stt.pixelFormat == MTLPixelFormatDepth32Float_Stencil8; sb = att_bytes(stt, pd.stencilAttachment.level) / (comb ? 5 : 1); if (stt == dt) db -= sb; }   // Combined format 5 bytes = depth 4 + stencil 1 (when only stencil is attached, only 1 is counted)
        if (dt) { g_sd[g_nsd] = (sd_att){0}; g_sd[g_nsd].ser = R.rt.depth->serial; g_sd[g_nsd].bytes = db; g_sd[g_nsd].lv = (uint32_t)pd.depthAttachment.level; g_sd[g_nsd].sl = (uint32_t)pd.depthAttachment.slice; g_sd[g_nsd].idx = -1; g_sd[g_nsd].depth = 1; g_sd[g_nsd].load = pd.depthAttachment.loadAction == MTLLoadActionLoad; g_sd[g_nsd].wrote = R.clear_d != 0; g_nsd++;
                  pd.depthAttachment.storeAction = MTLStoreActionUnknown; }
        if (stt) { g_sd[g_nsd] = (sd_att){0}; g_sd[g_nsd].ser = R.rt.depth->serial; g_sd[g_nsd].bytes = sb; g_sd[g_nsd].lv = (uint32_t)pd.stencilAttachment.level; g_sd[g_nsd].sl = (uint32_t)pd.stencilAttachment.slice; g_sd[g_nsd].idx = -1; g_sd[g_nsd].depth = 2; g_sd[g_nsd].load = pd.stencilAttachment.loadAction == MTLLoadActionLoad; g_sd[g_nsd].wrote = R.clear_s != 0; g_nsd++;
                   pd.stencilAttachment.storeAction = MTLStoreActionUnknown; }
    }
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
    {   // Pass info (same convention as the former per-draw fetch: colors take each attachment's base texture format; sample count takes the last color attachment, or depth if there is depth)
        uint64_t ser = g_pass.serial + 1; memset(&g_pass, 0, sizeof g_pass); g_pass.serial = ser;
        int physical_colors = MAX(R.rt.n, bare && !g_bare_native ? 1 : 0);
        for (int i = 0; i < physical_colors && i < 8; i++) { id<MTLTexture> t = pd.colorAttachments[i].texture;
            if (t) { g_pass.cfmt[i] = (uint32_t)t.pixelFormat; g_pass.samples = (uint32_t)t.sampleCount; } }
        id<MTLTexture> dt = R.rt.depth ? pd.depthAttachment.texture : nil, stt = R.rt.depth ? pd.stencilAttachment.texture : nil;   // Depth / stencil attachments are fetched separately (possibly only one of them is attached)
        g_pass.hd = dt && dt.pixelFormat != MTLPixelFormatStencil8; g_pass.hs = stt != nil;
        g_pass.dfmt = g_pass.hd ? (uint32_t)dt.pixelFormat : 0; g_pass.sfmt = stt ? (uint32_t)stt.pixelFormat : 0;
        if (dt || stt) g_pass.samples = (uint32_t)(dt ? dt : stt).sampleCount;
        pass_dims(pd, (NSUInteger)physical_colors, &g_pass.tw, &g_pass.th);
        if (rt_range_on()) { g_pass.reg_gen = reg0; g_pass.ser_gen = ser0; }
        if (rt_range_on()) { g_pass.range_dirty=dirty0; g_pass.range_color=pd.colorAttachments[0].texture; g_pass.range_depth=pd.depthAttachment.texture; }
        g_enc_pso = nil; g_enc_dss = nil; g_ed.ok = 0;
    }
    if (g_vis_on) {
        // Slots used up (one submission exceeds VIS_SLOTS counting passes): continue in a new overflow buffer. A ring buffer must not be retired (it is still used in rotation, and retiring removes it from the residency set); only a previous overflow buffer is
        if (!g_vis || g_vis_n >= VIS_SLOTS) { if (g_vis && !vis_in_ring(g_vis)) defer_obj_release(g_vis); g_vis = [g_dev newBufferWithLength:VIS_SLOTS * 8 options:MTLResourceStorageModeShared]; res_add(g_vis); g_vis_n = 0; }
        pd.visibilityResultBuffer = g_vis;
    }
    g_enc_vis = 0; g_enc_has_vis = g_vis_on;
    pg_render(pd, (NSUInteger)R.rt.n);
    { void td_note(id pd, NSUInteger nc); td_note(pd, (NSUInteger)R.rt.n);
      void ts_note(id pd, NSUInteger nc); ts_note(pd, (NSUInteger)R.rt.n);
      void ms_note(id pd, NSUInteger nc); ms_note(pd, (NSUInteger)R.rt.n); }
    ser_gate();
    pixel_history_begin(pd, (NSUInteger)R.rt.n);
    g_enc = enc_r_begin_pass(pd);
    args_dirty_all();
    g_stat_pass++;
    if (enc_label_on()) { id<MTLTexture> lt = pd.colorAttachments[0].texture ?: pd.depthAttachment.texture ?: pd.stencilAttachment.texture; g_el_w = (int)lt.width; g_el_h = (int)lt.height; g_el_draws = 0; g_el_key[0] = 0; }
    return g_enc;
}
// Empty-scissor precheck before opening a pass: the size of the selected level of the first valid target is an upper bound on the minimum bounds of the actual pass.
static void rt_dims(NSUInteger *tw, NSUInteger *th) {
    id<MTLTexture> t0 = nil; NSUInteger level = 0;
    for (int i = 0; i < R.rt.n && i < 8 && !t0; i++) if (R.rt.color[i]) {
        t0 = tex_mtl(R.rt.color[i]); level = R.rt.has_cview[i] && R.rt.cview[i].set_levels ? R.rt.cview[i].base_level : 0;
    }
    if (!t0 && R.rt.depth) { t0 = tex_mtl(R.rt.depth); level = R.rt.has_dview && R.rt.dview.set_levels ? R.rt.dview.base_level : 0; }
    if (!t0 && !R.rt.depth) {
        int bare = 1; for (int i = 0; i < R.rt.n; i++) bare &= R.rt.color[i] == NULL;
        if (bare) { bare_dims(tw, th); return; }
    }
    *tw = t0 ? mip_extent(t0.width, level) : 1; *th = t0 ? mip_extent(t0.height, level) : 1;
}
// There are pending clears but no draw in the pass: open a pass once to land the clears
static void rp_flush(void) {
    int pend = R.clear_d || R.clear_s;
    for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
    NSUInteger slices = !g_slice_replaying ? slice_target_count() : 1;
    if (pend && !g_enc && g_cb && slices > 1) {
        typeof(R) saved = R; g_slice_replaying = 1;
        for (uint32_t i = 0; i < slices; i++) { R = saved; slice_select(i); rp_begin_ex(1); rp_end(); }
        R = saved; memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
        g_slice_replaying = 0; g_selected_slice = 0;
        return;
    }
    if (pend && !g_enc && g_cb) rp_begin_ex(1);
    rp_end();
}
static int scissor_full(const tex_t *t, const view_t *v, int has_v) {
    NSUInteger level = has_v && v->set_levels ? v->base_level : 0;
    NSUInteger w = mip_extent(t->d.width, level), h = mip_extent(t->d.target == 0 || t->d.target == 3 ? 1 : t->d.height, level);
    return !R.has_sc || (R.sc.x <= 0 && R.sc.y <= 0 && (int64_t)R.sc.x + R.sc.w >= (int64_t)w && (int64_t)R.sc.y + R.sc.h >= (int64_t)h);
}
// Scissored zero clear (see OP_CLEAR_COLOR): land pending clears first, then copy from the zero buffer into the scissor rectangle of the attachment's level / slice in the replay stream's copy encoder. Multisample, 3D, depth / compressed formats are not handled (returns 0 and takes the old path)
static int clear_rect_zero(int i) {
    const tex_t *t = R.rt.color[i]; id<MTLTexture> mt = tex_mtl(t); if (!mt) return 0;
    MTLTextureType tt = mt.textureType;
    if (tt == MTLTextureType2DMultisample || tt == MTLTextureType2DMultisampleArray || tt == MTLTextureType3D || tt == MTLTextureTypeTextureBuffer) return 0;
    size_t bpp = mtl_pf_bytes(mt.pixelFormat); if (!bpp) return 0;
    const view_t *v = &R.rt.cview[i]; int hv = R.rt.has_cview[i];
    NSUInteger lv = hv && v->set_levels ? v->base_level : 0, sl = hv && v->set_layers ? v->min_layer : 0;
    long lw = (long)MAX(mt.width >> lv, 1u), lh = (long)MAX(mt.height >> lv, 1u);
    long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
    if (x1 > lw) x1 = lw; if (y1 > lh) y1 = lh;
    if (x1 <= x0 || y1 <= y0) return 1;
    size_t row = (size_t)(x1 - x0) * bpp, img = row * (size_t)(y1 - y0);
    id<MTLBuffer> zb = zero_buf(img); if (!zb) return 0;
    rp_flush();   // a pending full clear on the same attachment must land first, or it would overwrite this one
    enc_b_copy_buffer_texture(blit(), zb, 0, row, img, MTLSizeMake((NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0), 1), mt, sl, lv, MTLOriginMake((NSUInteger)x0, (NSUInteger)y0, 0));
    static uint64_t n; if (!n++) MLOG("剪裁清零：首例 %ldx%ld @%ld,%ld（目标 %lux%lu 级 %lu）", x1 - x0, y1 - y0, x0, y0, (unsigned long)mt.width, (unsigned long)mt.height, (unsigned long)lv);
    return 1;
}


// ================= Shaders / descriptor pool / pipelines / draws =================
static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;
// ---------- Shader mapping table: Switch control section hash → PC metallib (produced by dm/gfx/tools/shader_map.py) ----------
static NSMutableDictionary<NSString *, NSArray *> *g_shmap;
static NSString *g_msl_dir;
static NSString *exe_rel(const char *rel) {
    char buf[4096]; uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n)) return nil;
    char *dir = dirname(buf);
    return [NSString stringWithFormat:@"%s/%s", dir, rel];
}
// Shader mapping table (output of tools/shader_map.py): at runtime only 'Switch control section key → stage' is used; shaders are always translations of the Switch originals (tools/dmsh_build.py produces <key>_b.metallib + reflection offline)
static void shmap_load(void) {
    if (g_shmap) return;
    const char *e = getenv("DM_NVN_SHMAP");
    NSString *path = e ? @(e) : exe_rel("../games/" DM_GAME_ID "/shader_map.tsv");   // default: the current game profile folder (Mac: dm/games/<id>; on iOS main.m sets DM_NVN_SHMAP)
    NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    if (!txt) MDIE("读不到着色器映射表 %s（DM_NVN_SHMAP 可指定）", path.UTF8String);
    g_shmap = [NSMutableDictionary dictionary];
    for (NSString *ln in [txt componentsSeparatedByString:@"\n"]) {
        if (!ln.length || [ln hasPrefix:@"#"] || [ln hasPrefix:@"package"]) continue;
        NSArray *c = [ln componentsSeparatedByString:@"\t"];
        if (c.count > 3) g_shmap[c[3]] = c[2];
    }
    g_shmap_path = path;
    const char *m = getenv("DM_MSL_DIR");
    // Library directory = <DM_MSL_DIR or default cache>/b-<translation chain version>: the version comes from the game profile's dmsh_chain.h (tools/dmsh_chain.py); a host built from an old source tree only accepts libraries of the old version
    g_msl_dir = [NSString stringWithFormat:@"%@/b-%s", m ? @(m) : [NSString stringWithFormat:@"%s%s/build/cache/metal_shader_cache", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work"], DMSH_CHAIN];
    if (![NSFileManager.defaultManager fileExistsAtPath:g_msl_dir]) MLOG("【红】着色器库目录 %s 不存在：本源码树的翻译链版本 %s 还没出包（dm/gfx/tools/dmsh_build.py）", g_msl_dir.UTF8String, DMSH_CHAIN);
    NSString *cp = [[path stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"c1_full.tsv"];   // Same directory as the mapping table (Mac: dm/games/<id>; iOS: dm/ inside the App, copied along by build_ipa.sh)
    NSString *ct = cp ? [NSString stringWithContentsOfFile:cp encoding:NSUTF8StringEncoding error:nil] : nil;
    NSString *head = ct ? [[ct componentsSeparatedByString:@"\n"] firstObject] : nil;
    NSString *want = [NSString stringWithFormat:@"# chain b-%s", DMSH_CHAIN];
    if (!head || ![head hasPrefix:want]) {
        MLOG("【红】c1_full.tsv 清单过期或缺失，全部 c1 独占 64 KiB");
    } else {
        g_c1_full_valid = 1; g_c1_full = [NSMutableSet set];
        for (NSString *line in [ct componentsSeparatedByString:@"\n"]) if (line.length && ![line hasPrefix:@"#"]) [g_c1_full addObject:[line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]];
    }
    MLOG("着色器路线 B（Switch 原版翻译）：映射表 %lu 条（%s），metallib 目录 %s", (unsigned long)g_shmap.count, path.UTF8String, g_msl_dir.UTF8String);
    pso_learning_start();
    warm_start();
}

@interface NvShader : NSObject { @public id<MTLFunction> fn, tf_fn; uint16_t tf_stride[4]; nvn_refl rf; id<MTLArgumentEncoder> ae; NSUInteger arg_len; int stage, range_ro, afr, specialized, slice_variant, zero_image_variant, zero_image_aid, zero_image_reg; const char *builtin; uint64_t logged; char key[24]; id<MTLBuffer> c1buf; uint64_t c1_gpu; size_t c1off;
    id<MTLFunction> nonpoint_fn; id<MTLLibrary> nonpoint_lib; NSError *nonpoint_error; int nonpoint_tried;
    id<MTLFunction> min_reduction_fn; id<MTLLibrary> min_reduction_lib; int min_reduction_tried, min_reduction_last, min_reduction_notes;
    int min_reduction_bound, min_reduction_sid, min_reduction_tid, min_reduction_pso_failed;
    // The argument buffer encoded last time
    id<MTLBuffer> ab_buf; uint64_t ab_gpu; size_t ab_off; uint64_t ab_pass; uint32_t ab_binst;   // ab_pass: serial of the render pass that encoded this argument block - reused only within the same pass (the arena holding a block from another pass / another submission may already have been reclaimed and rewritten)
    id<MTLLibrary> bflib; int bflib_tried; NSMutableDictionary<NSString *, id<MTLFunction>> *bffn; } @end   // bflib: readable-tile version <key>_bf (merge B step 2)
@implementation NvShader @end
#include "nvn_pso_skey.h"
@interface NvProg : NSObject { @public NvShader *sh[6]; } @end
@implementation NvProg @end
// Warm-up entry: built in the background, waiting to be taken by the submit thread. vs / fs are strong references (in pso they are only raw pointers, and the background dictionary is released when the task ends)
@interface WarmItem : NSObject { @public int kind; NSString *key, *vs_key, *fs_key; NvShader *shader, *vs, *fs; pso_key pso; id<MTLFunction> fsfn; id object; } @end
@implementation WarmItem @end
static NSMutableDictionary<NSString *, NvShader *> *g_shcache;

static void c1_store(NvShader *o) {
    size_t len = o->rf.c1_len;
    NSString *guardKey = [NSString stringWithUTF8String:o->key];
    if ([guardKey hasSuffix:@"_b"]) guardKey = [guardKey substringToIndex:guardKey.length - 2];
    os_unfair_lock_lock(&g_c1_lock);   // Both the warm-up thread and the submit thread load shaders
    if (!g_c1_full_valid || [g_c1_full containsObject:guardKey]) {   // Runtime index not proven bounded: exclusive 64 KiB of all zeros (same as the original implementation)
        o->c1buf = [g_dev newBufferWithLength:C1_DECL_SIZE options:MTLResourceStorageModeShared]; o->c1_gpu = legacy_buffer_gpu(o->c1buf);
        if (!o->c1buf) { os_unfair_lock_unlock(&g_c1_lock); MDIE("c1 独占缓冲分配失败"); }
        res_add(o->c1buf);
        memset(o->c1buf.contents, 0, C1_DECL_SIZE); memcpy(o->c1buf.contents, o->rf.c1, len);
        g_c1_alloc_bytes += C1_DECL_SIZE; g_c1_data_bytes += len; g_c1_count++; g_c1_exclusive++;
        os_unfair_lock_unlock(&g_c1_lock);
        return;
    }
    size_t need = (len + 255u) & ~((size_t)255u);
    if (!g_c1_banks) g_c1_banks = [NSMutableArray array];
    if (!g_c1_banks.count || g_c1_bank_off + need + 256u + C1_DECL_SIZE > C1_BANK_SIZE) {
        id<MTLBuffer> b = [g_dev newBufferWithLength:C1_BANK_SIZE options:MTLResourceStorageModeShared];
        if (!b) { os_unfair_lock_unlock(&g_c1_lock); MDIE("c1 缓冲块分配失败"); }
        res_add(b);
        memset(b.contents, 0, C1_BANK_SIZE);
        [g_c1_banks addObject:b];
        g_c1_bank_off = 0;
        g_c1_alloc_bytes += C1_BANK_SIZE;
    }
    id<MTLBuffer> b = g_c1_banks.lastObject;
    size_t off = g_c1_bank_off;
    memcpy((uint8_t *)b.contents + off, o->rf.c1, len);
    g_c1_bank_off += need + 256u;
    g_c1_data_bytes += len;
    g_c1_count++;
    o->c1buf = b;
    o->c1_gpu = legacy_buffer_gpu(b);
    o->c1off = off;
    os_unfair_lock_unlock(&g_c1_lock);
}

static NSMutableArray<WarmItem *> *g_warm_items;
static os_unfair_lock g_warm_lock = OS_UNFAIR_LOCK_INIT;
static _Atomic int g_warm_ready;
static int g_warm_started;
static uint64_t g_warm_t0;
static _Atomic int g_warm_running;   // Background warm-up has not finished yet (the loading screen on entering a run waits a little longer based on this, see mtl_warm_wait)
static uint64_t g_warm_shaders, g_warm_rps, g_warm_cps, g_warm_skip_fz;
static uint64_t g_warm_claim_shaders, g_warm_claim_rps, g_warm_claim_cps;
static id<MTLFunction> bf_fn(NvShader *fs, const int *fx, uint8_t fbmask, const uint8_t *lmap);
static int rt_range_bindings_ro(NSDictionary *j);
static FILE *g_pso_record; static int g_pso_record_checked;
static FILE *pso_record_file(void) {
    if (g_pso_record_checked) return g_pso_record;
    g_pso_record_checked = 1;
    const char *path = getenv("DM_PSO_RECORD");
    if (path && *path) g_pso_record = fopen(path, "a");
    return g_pso_record;
}
static void pso_record(const char *fmt, ...) {
    FILE *f = pso_record_file();
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fflush(f);
}
// Build the library from a .metallib file without permanently occupying a file descriptor: newLibraryWithURL keeps that file open for as long as the library object lives (one per game shader - of the 258 descriptors the iOS app permanently holds in a run, 213 are these,
// while the default soft limit of an iOS process is 256; new shaders created in other acts raise it further). Here the file is mapped into memory and closed immediately, and the library is built from the in-memory data; the mapping is released when the library's data object is freed
static id<MTLLibrary> lib_from_file(NSString *path, NSError **err) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size <= 0) { if (fd >= 0) close(fd); if (err) *err = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"打不开 %@", path]}]; return nil; }
    size_t len = (size_t)st.st_size; void *m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if (m == MAP_FAILED) { if (err) *err = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"映射失败 %@", path]}]; return nil; }
#ifdef DM_SHELL
    // Shell: when the user's computer has no Apple shader compiler (the no-compiler mode of tools/d2rm_pack.py, gfx/tools/nometal_run.py), the "library files" contain
    // shader source rather than compiled libraries (a compiled library starts with MTLB). They are compiled here on the spot; the system caches the compiled result, and from the second launch on the same shader no longer costs this time.
    // Measured on a real device (cold): about 80 ms each on average, about 450 ms for the longest single one; one session uses several hundred.
    if (len < 4 || memcmp(m, "MTLB", 4)) { NSString *src = [[NSString alloc] initWithBytes:m length:len encoding:NSUTF8StringEncoding]; munmap(m, len);
        if (!src) { if (err) *err = [NSError errorWithDomain:NSPOSIXErrorDomain code:EILSEQ userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"既不是编好的库也不是源码 %@", path]}]; return nil; }
        static _Atomic int n; static _Atomic long long us; CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
        id<MTLLibrary> l = [g_dev newLibraryWithSource:src options:nil error:err]; long long dt = (long long)((CFAbsoluteTimeGetCurrent() - t0) * 1e6);
        int k = atomic_fetch_add(&n, 1) + 1; long long tot = atomic_fetch_add(&us, dt) + dt;
        if (k == 1 || k % 100 == 0 || dt >= 300000) MLOG("着色器现编：第 %d 个 %.0f ms，累计 %.1f 秒", k, dt / 1e3, tot / 1e6);
        return l; }
#endif
    dispatch_data_t d = dispatch_data_create(m, len, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ munmap(m, len); });
    return [g_dev newLibraryWithData:d error:err];
}
// NVN depth mode.  The offline translator emits both entry points for vertex
// shaders: `entry` converts the default GL-style [-w,+w] depth range to Metal,
// while `entry_z` keeps the already-[0,w] form selected by
// nvnDeviceSetDepthMode.  The mode is set before shaders are first loaded.
static _Atomic int g_depth_zero, g_vs_loaded;
static NSString *nonpoint_digest(NSData *data);
#include "nvn_layered_slices.h"
#include "nvn_zero_image_library.h"
#include "nvn_sampler_limit.h"
static id<MTLLibrary> bounded_atomic_library(NSString *key, NSError **error) {
    NSString *root = [g_msl_dir stringByAppendingPathComponent:@"atomic-v1"];
    NSData *meta = [NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:[key stringByAppendingString:@".json"]]];
    NSDictionary *record = meta ? [NSJSONSerialization JSONObjectWithData:meta options:0 error:nil] : nil;
    NSData *source = [NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:[key stringByAppendingString:@".metal"]]];
    NSData *base = [NSData dataWithContentsOfFile:[g_msl_dir stringByAppendingPathComponent:[key stringByAppendingString:@".metallib"]] options:NSDataReadingMappedIfSafe error:nil];
    if (![record[@"schema"] isEqual:@1] || ![record[@"chain"] isEqual:@DMSH_CHAIN] || ![record[@"kind"] isEqual:@"bounded-image-atomics-v1"] || ![record[@"source_sha256"] isEqual:nonpoint_digest(source)] || ![record[@"library_sha256"] isEqual:nonpoint_digest(base)]) {
        if (error) *error = [NSError errorWithDomain:@"NVNLegacyAtomics" code:1 userInfo:@{NSLocalizedDescriptionKey:@"Missing or mismatched bounded image-atomic source; export_legacy_atomics.py is required"}];
        return nil;
    }
    NSString *src = [[NSString alloc] initWithData:source encoding:NSUTF8StringEncoding];
    MTLCompileOptions *options = [MTLCompileOptions new]; options.languageVersion = MTLLanguageVersion3_0; options.preserveInvariance = YES; options.fastMathEnabled = NO;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:src options:options error:error];
    if (library) MLOG("Bounded image-atomic variant %s: image-coordinate guards and array-layer addressing enabled", key.UTF8String);
    return library;
}
void mtl_set_depth_zero(int on) {
    if (atomic_exchange(&g_depth_zero, on) == on) return;
    MLOG("深度模式：%s", on ? "near=0，顶点着色器使用 entry_z" : "near=-w（GL），顶点着色器使用 entry");
    int n = atomic_load(&g_vs_loaded);
    if (n) MLOG("【红】已加载 %d 个顶点着色器后才改变深度模式；这些着色器保留旧模式", n);
}
#define NVN_STORAGE_ACCESS_LOG(...) MLOG(__VA_ARGS__)
#include "nvn_storage_access_metal.h"
#undef NVN_STORAGE_ACCESS_LOG
static NvShader *shader_load(NSString *pc, int stage) {
    NSError *e = nil;
    NSString *lp = [NSString stringWithFormat:@"%@/%@.metallib", g_msl_dir, pc];
    id<MTLLibrary> lib = lib_from_file(lp, &e);
    NSData *jd = [NSData dataWithContentsOfFile:[NSString stringWithFormat:@"%@/%@.json", g_msl_dir, pc]];
    NSDictionary *j = jd ? [NSJSONSerialization JSONObjectWithData:jd options:0 error:nil] : nil;
    if (!lib || !j) { MLOG("【红】加载着色器失败 %s：%s", pc.UTF8String, e ? e.localizedDescription.UTF8String : "无反射 json"); return nil; }
    if (![j[@"chain"] isEqual:@DMSH_CHAIN]) { MLOG("【红】着色器 %s 的翻译链版本 %s 与本宿主期望的 %s 不符", pc.UTF8String, [[j[@"chain"] description] UTF8String], DMSH_CHAIN); return nil; }
    NvShader *o = [NvShader new];
    if (!g_layered_native) {
        lib = layered_slice_library(pc, lib, jd, &o->slice_variant, &e);
        if (!lib) { MLOG("[RED] Shader %s per-slice compatibility unavailable: %s", pc.UTF8String, e.localizedDescription.UTF8String); return nil; }
        o->specialized = o->slice_variant;
    }
    if (g_dev.argumentBuffersSupport == MTLArgumentBuffersTier1) {
        NSDictionary *limited_j = j;
        int sampler_limit_variant = 0;
        lib = sampler_limit_library(pc, lib, jd, j, o->specialized, &limited_j,
                                    &sampler_limit_variant, &e);
        if (!lib) {
            MLOG("[RED] Shader %s Tier 1 sampler compatibility unavailable: %s",
                 pc.UTF8String, e.localizedDescription.UTF8String);
            return nil;
        }
        j = limited_j;
        if (sampler_limit_variant) o->specialized = 1;
    }
    char err[160];
    if (nvn_refl_from_json(j, &o->rf, err, sizeof err)) { MLOG("【红】着色器 %s 反射转换失败：%s", pc.UTF8String, err); return nil; }
    if (stage == 1 && !g_fragment_image_native) {
        int images = 0; for (int i = 0; i < o->rf.nb; i++) images += o->rf.b[i].kind == NVN_RB_IMG && o->rf.b[i].rw;
        if (images) {
            lib = zero_image_library(pc, lib, jd, &o->zero_image_variant, &o->zero_image_aid, &o->zero_image_reg, &e);
            if (!lib) { MLOG("[RED] Shader %s fragment image-write compatibility unavailable: %s", pc.UTF8String, e.localizedDescription.UTF8String); return nil; }
            int matched = 0;
            for (int i = 0; i < o->rf.nb; i++) if (o->rf.b[i].kind == NVN_RB_IMG && o->rf.b[i].rw && o->rf.b[i].aid == o->zero_image_aid && o->rf.b[i].reg == o->zero_image_reg && o->rf.b[i].n <= 1) {
                o->rf.b[i].kind = NVN_RB_IMGA; o->rf.b[i].rw = 1; matched++;
            }
            if (images != 1 || matched != 1 || o->zero_image_reg >= 32 || o->zero_image_reg < 0) {
                MLOG("[RED] Shader %s zero-image reflection does not match the proven single image", pc.UTF8String); return nil;
            }
            o->specialized = 1;
        }
    }
    for (int i = 0; i < o->rf.nb && !o->zero_image_variant; i++) if (o->rf.b[i].kind == NVN_RB_IMGA) {
        lib = bounded_atomic_library(pc, &e);
        if (!lib) { MLOG("[RED] Shader %s bounded image-atomic variant failed: %s", pc.UTF8String, e.localizedDescription.UTF8String); return nil; }
        break;
    }
    if (rt_range_on()) o->range_ro=rt_range_bindings_ro(j);
    NSString *entry = j[@"entry"];
    if (stage == 0) {
        atomic_fetch_add(&g_vs_loaded, 1);
        if (atomic_load(&g_depth_zero)) {
            if (j[@"entry_z"]) entry = j[@"entry_z"];
            else MLOG("【红】顶点着色器 %s 没有 entry_z；仍使用 GL 深度入口", pc.UTF8String);
        }
    }
    o->fn = [lib newFunctionWithName:entry]; o->stage = stage; strlcpy(o->key, pc.UTF8String, sizeof o->key);
    if (j[@"entry_tf"]) {
        o->tf_fn = [lib newFunctionWithName:j[@"entry_tf"]];
        NSArray *strides = j[@"tf_strides"];
        for (NSUInteger b = 0; b < 4 && b < strides.count; b++) o->tf_stride[b] = (uint16_t)[strides[b] unsignedIntValue];
        if (!o->tf_fn) MLOG("[RED] shader %s has no transform-feedback entry point %s in its library", pc.UTF8String, [j[@"entry_tf"] UTF8String]);
    }
    if (o->fn.functionConstantsDictionary.count) {
        MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
        NSError *functionError = nil;
        id<MTLFunction> specialized = [lib newFunctionWithName:entry constantValues:values error:&functionError];
        if (!specialized) { MLOG("[RED] Shader %s default function-constant specialization failed: %s", pc.UTF8String, functionError.localizedDescription.UTF8String); return nil; }
        o->fn = specialized; o->specialized = 1;
        MLOG("Shader %s uses default function-constant specialization", pc.UTF8String);
    }
    if (o->rf.nb) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        MTLArgument *argument = nil;
        @try { o->ae = nvn_storage_access_encoder(o->fn, o->rf.abi, &argument); }
        @catch (NSException *exception) {
            MLOG("[RED] Shader %s stage %d argument encoder unavailable: %s: %s", o->key, stage,
                 exception.name.UTF8String, exception.reason.UTF8String);
            return nil;
        }
        int max_aid = -1;
        for (int i = 0; i < o->rf.nb; i++) max_aid = MAX(max_aid, o->rf.b[i].aid);
        o->arg_len = o->ae.encodedLength;
        if (!o->ae || !o->arg_len || (g_legacy_direct_arguments &&
            (o->arg_len != (NSUInteger)(8 * (max_aid + 1)) || (o->arg_len & 7))))
            MDIE("着色器 %s 参数块布局异常：encodedLength=%lu maxAid=%d", o->key, (unsigned long)o->arg_len, max_aid);
        if (g_legacy_direct_arguments) nvn_storage_access_promote(&o->rf, argument, o->arg_len, o->key, o->stage);
        else nvn_argument_private_access(&o->rf, argument);
#pragma clang diagnostic pop
    }
    o->fn.label = pc;
    if (o->rf.has_c1) c1_store(o);
    return o;
}

static int stage_of(NSString *name) {
    if ([name isEqualToString:@"vertex"]) return 0;
    if ([name isEqualToString:@"pixel"]) return 1;
    if ([name isEqualToString:@"hull"]) return 3;
    if ([name isEqualToString:@"domain"]) return 4;
    if ([name isEqualToString:@"compute"]) return 5;
    return -1;
}
#define NVN_RT_RANGE_RO_IMPL 1
// Private continuation accepts only B reflection that is explicitly RO; a missing rw on sbuf does not let the default 0 count as proof; does not change the existing reflection / translation chain.
static int rt_range_bindings_ro(NSDictionary *j) {
    if (![j[@"route"] isEqual:@"B"] || ![j[@"bindings"] isKindOfClass:[NSArray class]] ||
        ![j[@"drv_reads"] isKindOfClass:[NSDictionary class]] || [j[@"drv_reads"] count]) return 0;
    for (NSDictionary *b in j[@"bindings"]) {
        if (![b isKindOfClass:[NSDictionary class]] || ![b[@"kind"] isKindOfClass:[NSString class]] || ![b[@"msl_type"] isKindOfClass:[NSString class]]) return 0;
        NSString *k=b[@"kind"], *t=b[@"msl_type"];
        if (b[@"rw"] && (![b[@"rw"] isKindOfClass:[NSNumber class]] || [b[@"rw"] boolValue])) return 0;
        if ([k isEqual:@"cbuf"] || [k isEqual:@"c1"]) { if (![t hasPrefix:@"constant "]) return 0; }
        else if ([k isEqual:@"sbuf"]) { if (![b[@"rw"] isKindOfClass:[NSNumber class]] || [b[@"rw"] boolValue] || ![t hasPrefix:@"const device "]) return 0; }
        else if ([k isEqual:@"samp"] || [k isEqual:@"sampt"]) { if (![t isEqual:@"sampler"]) return 0; }
        else if ([k isEqual:@"tex"] || [k isEqual:@"tbuf"]) {
            if (![t hasPrefix:@"texture"] || ([t containsString:@"access::"] && ![t containsString:@"access::sample>"] && ![t containsString:@"access::read>"])) return 0; }
        else return 0;
    }
    return 1;
}
static int rt_range_pr_ro(NvProg *p) {
    if (!p || !p->sh[0] || !p->sh[1]) return 0;
    for (int k=0;k<6;k++) if (p->sh[k] && (k>1 || p->sh[k]->stage != k || !p->sh[k]->range_ro || p->sh[k]->rf.drv_read[0] || p->sh[k]->rf.drv_read[2])) return 0;
    return 1;
}
static NvShader *shader_get(NSString *pc, int stage) {
    if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
    NvShader *o = g_shcache[pc];
    if (o) return o;
    uint64_t bt = bp_now(); o = shader_load(pc, stage);
    if (!o) return nil;
    g_shcache[pc] = o;
    pso_record("S\t%s\t%d\n", pc.UTF8String, stage);
    pso_user_record("S\t%s\t%d\n", pc.UTF8String, stage);
    g_bp_lib_n++; g_bp_lib_ns += bp_now() - bt;
    return o;
}
static id<MTLLibrary> g_builtin_lib;
static NSMutableDictionary<NSString *, NvShader *> *g_builtin;
static NvShader *builtin_get(const char *ctl) {
    for (unsigned i = 0; i < sizeof k_nvn_builtin_map / sizeof *k_nvn_builtin_map; i++) {
        if (strcmp(ctl, k_nvn_builtin_map[i].ctl)) continue;
        if (!g_builtin) g_builtin = [NSMutableDictionary dictionary];
        NvShader *o = g_builtin[@(ctl)];
        if (o) return o;
        if (!g_builtin_lib) {
            NSError *e = nil; g_builtin_lib = [g_dev newLibraryWithSource:@(k_nvn_builtin_msl) options:nil error:&e];
            if (!g_builtin_lib) MDIE("内嵌计算核 MSL 编译失败：%s", e.localizedDescription.UTF8String);
        }
        o = [NvShader new]; o->fn = [g_builtin_lib newFunctionWithName:@(k_nvn_builtin_map[i].fn)]; o->stage = 5; o->builtin = k_nvn_builtin_map[i].fn;
        g_builtin[@(ctl)] = o;
        return o;
    }
    return nil;
}
static __unsafe_unretained NvProg *prog_get(const prog_t *cp) {   // Called only on the submit thread
    prog_t *p = (prog_t *)cp;
    static const prog_t *lp; static uint64_t lser, lgen; static NvProg *lpr;   // Last result: same object, same serial and unchanged serial table means it is used directly
    uint64_t gen = g_ser_gen;
    if (p == lp && p->serial == lser && gen == lgen && lpr) return lpr;
    __unsafe_unretained NvProg *have = p->serial ? serial_get(p->serial) : nil;
    if (have) { lp = p; lser = p->serial; lgen = gen; lpr = have; return have; }
    shmap_load();
    NvProg *o = [NvProg new];
    for (unsigned i = 0; i < p->nstage; i++) {
        // Key = sha256(first 0x880 bytes of the control section + code area): many of Mario's shaders share a control section but differ in code (dm/gfx/tools/shader_split.py). It was computed at SetShaders (hle_nvn.c): by now the game may have freed the control section
        uint8_t md[8]; prog_key(p, i, md);
        NSString *k = [NSString stringWithFormat:@"%02x%02x%02x%02x%02x%02x%02x%02x", md[0], md[1], md[2], md[3], md[4], md[5], md[6], md[7]];
        NSString *stageName = g_shmap[k];
        if (!stageName) {   // Not in the package: look up the compute kernels embedded in the main body (nvn_builtin.h)
            NvShader *b = builtin_get(k.UTF8String);
            if (b) { o->sh[5] = b; continue; }
            MLOG("【红】着色器 %s 不在映射表里（DM_NVN_SHDUMP=目录 可把控制段与代码区存盘）", k.UTF8String);
            // DM_NVN_SHDUMP=directory: shaders missing from the map are saved as <key>.ctl (0x880-byte control section) and <key>.code (code area, length from control +0x704), to trace their source / add them to the map offline
            static const char *shd; static int shd_init; if (!shd_init) { shd = getenv("DM_NVN_SHDUMP"); shd_init = 1; }
            if (shd) { char path[1024]; FILE *f; uint32_t ncode; memcpy(&ncode, (const uint8_t *)p->sd[i].control + 0x704, 4);   // with SHDUMP set the control section is the host's copy
                snprintf(path, sizeof path, "%s/%s.ctl", shd, k.UTF8String); if ((f = fopen(path, "wb"))) { fwrite(p->sd[i].control, 1, 0x880, f); fclose(f); }
                snprintf(path, sizeof path, "%s/%s.code", shd, k.UTF8String);
                if (p->sd[i].data && ncode && ncode < (1u << 24) && (f = fopen(path, "wb"))) { fwrite((const void *)(uintptr_t)p->sd[i].data, 1, ncode, f); fclose(f); } }
            continue;
        }
        int st = stage_of(stageName);
        NvShader *sh = st >= 0 ? shader_get([k stringByAppendingString:@"_b"], st) : nil;
        if (sh) o->sh[st] = sh; else MLOG("【红】着色器 %s（%s）加载失败：先跑 dm/gfx/tools/dmsh_build.py 出包", k.UTF8String, stageName.UTF8String);
    }
    if (p->serial) serial_put(p->serial, o);
    return o;
}
void mtl_prog_release(prog_t *p) { if (p->serial) serial_del(p->serial); }

// ---------- Descriptor pool: texture ID / sampler ID → object ----------
// At registration the whole texture object is snapshotted (NVN semantics: the descriptor has been written into the pool); view: the MTLTexture for that ID (a view or the base texture, held via CFBridgingRetain) and the serial-table / guest-write generations at the time it was obtained
typedef struct { tex_t t; int ok; view_t v; int has_v; void *view; uint64_t view_id, view_rid, view_ser, view_dirty; int view_pkg;   // view_pkg: CPU-written (packaged); only those depend on guest writes (view_dirty)
                 void *alt, *alt_of; uint64_t alt_rid; uint8_t alt_type;   // alt: compatible view used when the shader's declared type differs from view (tex_fit), alt_of = the view it was built from
                 void *ar, *ar_of; uint64_t ar_rid; } treg_t;              // ar: the alpha-from-red view for the profile's alpha_from_red shaders (tex_alpha_red), ar_of = the view it was built from
static treg_t *g_treg; static int g_ntreg;
// Lock-free copy of texture ID → serial (the replay hot path only needs the serial: store lookahead index, binding set of the clear-only hoist): texture IDs are 20 bits, fixed 1M entries (BSS, only touched pages are used),
// release write at registration, relaxed atomic read on the reader side, without entering g_rmu (the former h_ser locked and unlocked on every lookup, thousands of times per frame in the camp)
static _Atomic uint64_t g_tser[1 << 20];
static samp_par *g_sreg; static uint8_t *g_sreg_ok; static void **g_sview; static uint64_t *g_srid; static int g_nsreg;   // g_sview: sampler ID → MTLSamplerState (retained)
// Memory pool Finalize: for texture IDs whose registered texture lives on this pool, drop the cached MTLTexture (view or base texture). The cache used to be released only when the ID was registered again,
// so a linear texture built directly on a pool kept the whole pool buffer alive (6 to 17 finalized 16 MB pool buffers stayed alive in Engage's title demo). If the ID is still used, the next lookup rebuilds it on the slow path
static void treg_drop_ar(treg_t *r) { if (r->ar) { defer_replay_view_unref((__bridge id)r->ar); CFRelease(r->ar); r->ar = r->ar_of = NULL; r->ar_rid = 0; } }
static void treg_drop_pool_views(const void *pool) {
    pthread_mutex_lock(&g_rmu);
    for (int i = 0; i < g_ntreg; i++)
        if (g_treg[i].ok && (const void *)g_treg[i].t.pool == pool) {
            // The base view may also be referenced by independent upload or
            // present command buffers, neither of which advances g_fence.
            if (g_treg[i].view) { defer_obj_unref((__bridge id)g_treg[i].view); CFRelease(g_treg[i].view); g_treg[i].view = NULL; g_treg[i].view_id = g_treg[i].view_rid = 0; }
            // alt/ar are replay-only derived views. Both retain their parent;
            // leaving alt attached here pins the finalized pool even after the
            // base cache entry is gone.
            if (g_treg[i].alt) { defer_replay_view_unref((__bridge id)g_treg[i].alt); CFRelease(g_treg[i].alt); g_treg[i].alt = g_treg[i].alt_of = NULL; g_treg[i].alt_rid = 0; g_treg[i].alt_type = 0; }
            treg_drop_ar(&g_treg[i]); }
    pthread_mutex_unlock(&g_rmu);
    defer_replay_view_drain_safe_point();
}
void mtl_reg_tex(int tid, const tex_t *t, const view_t *v) {
    int id_ = tid; if (id_ < 0) return;
    pthread_mutex_lock(&g_rmu);
    if (id_ >= g_ntreg) { int n = id_ + 1024; g_treg = realloc(g_treg, n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    g_reg_gen++;
    // Keep the base view conservative: a present/readback/upload CB may still
    // hold an unsafe reference and those independent CBs do not signal g_done.
    if (g_treg[id_].view) { defer_obj_unref((__bridge id)g_treg[id_].view); CFRelease(g_treg[id_].view); g_treg[id_].view = NULL; g_treg[id_].view_id = g_treg[id_].view_rid = 0; }
    if (g_treg[id_].alt) { defer_replay_view_unref((__bridge id)g_treg[id_].alt); CFRelease(g_treg[id_].alt); g_treg[id_].alt = g_treg[id_].alt_of = NULL; g_treg[id_].alt_rid = 0; }
    treg_drop_ar(&g_treg[id_]);
    g_treg[id_].ok = t != NULL; if (t) g_treg[id_].t = *t; g_treg[id_].has_v = v != NULL; if (v) g_treg[id_].v = *v;
    if (id_ < (1 << 20)) atomic_store_explicit(&g_tser[id_], t ? t->serial : 0, memory_order_release);
    pthread_mutex_unlock(&g_rmu);
    defer_replay_view_drain_safe_point();
    {   // DM_NVN_TEXINFO=WxH also logs registrations of textures of that size: the channel swizzle from the builder and from the view
        static int ti = -1; static uint32_t tw, th; static uint64_t n;
        if (ti < 0) { const char *e = getenv("DM_NVN_TEXINFO"); ti = e && sscanf(e, "%ux%u", &tw, &th) == 2; }
        if (ti && t && t->d.width == tw && t->d.height == th && (n++ < 40 || n % 600 == 0))
            MLOG("TEXINFO 登记 id %d ser=%llu 格式=%#x 建时重排 %d:%d%d%d%d 视图 %d 视图重排 %d:%d%d%d%d 视图格式 %d:%#x", tid, (unsigned long long)t->serial, t->d.format,
                 t->set_swz, t->swz & 7, t->swz >> 3 & 7, t->swz >> 6 & 7, t->swz >> 9 & 7, v != NULL, v ? v->set_swizzle : 0, v ? v->swz[0] : 0, v ? v->swz[1] : 0, v ? v->swz[2] : 0, v ? v->swz[3] : 0,
                 v ? v->set_format : 0, v ? v->format : 0);
    }
}
void mtl_reg_samp(int sid, const samp_par *sp) {
    int id_ = sid; if (id_ < 0) return;
    pthread_mutex_lock(&g_rmu);
    if (id_ >= g_nsreg) { int n = id_ + 256; g_sreg = realloc(g_sreg, n * sizeof *g_sreg); g_sreg_ok = realloc(g_sreg_ok, n); g_sview = realloc(g_sview, n * sizeof *g_sview); g_srid = realloc(g_srid, n * sizeof *g_srid);
        memset(g_sreg_ok + g_nsreg, 0, n - g_nsreg); memset(g_sview + g_nsreg, 0, (n - g_nsreg) * sizeof *g_sview); memset(g_srid + g_nsreg, 0, (n - g_nsreg) * sizeof *g_srid); g_nsreg = n; }
    if (g_sview[id_]) {
        static unsigned replacements;
        if (replacements++ < 128) MLOG("Sampler replacement frame=%llu sid=%d old-rid=%#llx lod=%g..%g bias=%g; GPU references retained by submission", (unsigned long long)atomic_load(&g_cur_frame), id_, (unsigned long long)g_srid[id_], sp->lod_min, sp->lod_max, sp->lod_bias);
        CFRelease(g_sview[id_]); g_sview[id_] = NULL; g_srid[id_] = 0;
    }
    g_reg_gen++;
    g_sreg[id_] = *sp; g_sreg_ok[id_] = 1;
    pthread_mutex_unlock(&g_rmu);
}
// Handles of deferred binds (BindTexturesDeferred) are read at execution: the record keeps the address of the handles (see hle_nvn.c); every reader of a bind record refreshes it first. Command memory is writable, so it is updated in place
static inline void bh_fresh(const p_bind_handles *b) {
    if (b->defer && b->src && b->n > 0 && b->n <= 32) memcpy((void *)b->h, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n);
}
static MTLTextureType ttype(int target, int fallback) {
    switch (target) { case 0: return MTLTextureType2D; case 1: case 7: return MTLTextureType2D; case 2: return MTLTextureType3D;   // 1D as one-row 2D (see where textures are created)
        case 3: return MTLTextureType2DArray; case 4: return MTLTextureType2DArray; case 5: return MTLTextureType2DMultisample;
        case 6: return MTLTextureType2DMultisampleArray; case 8: return MTLTextureTypeCube; case 9: return MTLTextureTypeCubeArray; }
    return (MTLTextureType)fallback;
}
// Out-of-pool texture ID = empty slot: the game itself uses GetSeparateTextureHandle(0xFFFFF) to get a handle and bind it to unused slots (measured call site main+0x1746820,
// texture pool capacity 15256) and never registers that ID. Read 0 per D3D / empty-slot semantics (the same all-zero dummy textures as unbound), no failure; unregistered IDs inside the pool are still reported a failure
static int g_tpool_n; static uint64_t g_stat_tex_oob;
// NVN window origin. In lower-left mode (GL convention, NVN's default) NDC y = -1 lands on a render target's first memory row and window y grows with the row; Metal puts NDC y = +1 on the first row.
// Viewport and scissor rectangles, fragment coordinates, copies and texture coordinates all address the same memory rows in both conventions, so only the direction inside the viewport differs:
// lower-left draws get a viewport of negative height (and the opposite front-face winding), and the window image, stored bottom-up, is flipped at present (and in raw screenshots).
// Link's Awakening keeps the default: read as upper-left, every texture it renders and samples came out consistently flipped and cancelled out, except where the game compensates for the
// convention itself (text rendered into textures for the title and the menus came out upside down, and a MIRROR_Y copy of the frame)
static _Atomic int g_origin_ll;
void mtl_set_origin_lower_left(int on) { if (atomic_exchange(&g_origin_ll, on) != on) MLOG("窗口原点：%s", on ? "左下（GL，视口倒向、绕向反、出图翻转）" : "左上"); }
void mtl_set_tex_pool_n(int n) { g_tpool_n = n; }
static id<MTLTexture> tex_by_id(int tid) {   // Caller holds g_rmu
    if (g_tpool_n && tid >= g_tpool_n) { if (!g_stat_tex_oob++) MLOG("池外贴图号 %d（池容量 %d）按空槽处理：读出 0", tid, g_tpool_n); return nil; }
    if (tid < 0 || tid >= g_ntreg || !g_treg[tid].ok) { RED_ONCE(10, "贴图号 %d 未登记", tid); return nil; }
    treg_t *rp = &g_treg[tid];
    uint64_t sg = g_ser_gen, dg = nvn_dirty_now();   // Take the generations first and then look up: if a change happens in between, the next time must take the slow path
    if (rp->view && rp->view_ser == sg && (!rp->view_pkg || rp->view_dirty == dg)) return (__bridge id<MTLTexture>)rp->view;
    id<MTLTexture> v = (__bridge id<MTLTexture>)rp->view;
    // Guest writes (nvn_dirty_now) matter only to CPU-written textures: a texture that becomes CPU-written later bumps g_ser_gen once (mtl_tex_cpu_written), which sends its ID through here again.
    // Before, every guest write to any protected page sent every texture ID, render targets included, down this path
    rp->view_pkg = (rp->t.has_layout || tex_cpu_written(rp->t.serial)) && rp->t.mem;
    if (v) {   // The serial table or guest memory has changed: check whether the base texture was re-uploaded and replaced; if so the view is invalidated
        id<MTLTexture> cur = tex_mtl(&rp->t);
        if (cur == v || v.parentTexture == cur) { rp->view_ser = sg; rp->view_dirty = dg; return v; }
        defer_obj_unref(v); CFRelease(rp->view); rp->view = NULL; rp->view_rid = 0; v = nil;
    }
    treg_t r = *rp;
    id<MTLTexture> base = tex_mtl(&rp->t);
    if (!base) return nil;
    v = base;
    // Channel swizzle: the view's if set, otherwise the one from texture building; identity builds no view (SMBW's font cache is (1 1 1 R): without the swizzle glyphs become white blocks)
    uint8_t sw[4] = {MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha}; int need_sw = 0;
    if (r.has_v && r.v.set_swizzle) memcpy(sw, r.v.swz, 4);
    else if (r.t.set_swz) for (int c = 0; c < 4; c++) sw[c] = (uint8_t)(r.t.swz >> (3 * c) & 7);
    // Format conversion is applied before the builder/view channel selection. RGBX alpha is 1;
    // packed RGB formats may swap red/blue even when the explicit channel selection is identity.
    static int format_swizzle = -1;
    if (format_swizzle < 0) { const char *e = getenv("DM_NVN_FORMAT_SWIZZLE"); format_swizzle = e ? strcmp(e, "0") != 0 : DM_GAME_PROGRAM_ID == 0x010015100B514000ull; }
    uint8_t format_sw[4] = {NVN_SWZ_R, NVN_SWZ_G, NVN_SWZ_B, NVN_SWZ_A}; uint32_t ignored_pf;
    if (format_swizzle && !nvn_fmt_to_mtl(r.has_v && r.v.set_format ? r.v.format : r.t.d.format, &ignored_pf, format_sw)) nvn_fmt_to_mtl(r.t.d.format, &ignored_pf, format_sw);
    for (int c = 0; c < 4; c++) {
        if (sw[c] > MTLTextureSwizzleAlpha) sw[c] = (uint8_t)(MTLTextureSwizzleRed + c);
        if (sw[c] >= MTLTextureSwizzleRed) sw[c] = format_sw[sw[c] - MTLTextureSwizzleRed];
        if (sw[c] != MTLTextureSwizzleRed + c) need_sw = 1;
    }
    if (format_swizzle) { static unsigned reports; int changed = 0; for (int c = 0; c < 4; c++) changed |= format_sw[c] != NVN_SWZ_R + c;
        if (changed && reports++ < 16) MLOG("[color] 格式通道修正 tid=%d nvn=%#x view=%#x 最终=%u,%u,%u,%u", tid, r.t.d.format, r.has_v && r.v.set_format ? r.v.format : 0, sw[0],sw[1],sw[2],sw[3]); }
    // STENCIL depth/stencil mode means sampling the stencil plane, not the
    // depth plane.  Z-A's deferred-lighting shader reads this view as raw
    // float bits and tests stencil bit 6; binding depth instead produces the
    // characteristic black/white bands that follow scene depth.
    int want_st = r.has_v && r.v.set_dsmode ? r.v.dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL : r.t.stencil;
    int stv = want_st && base.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
    if (want_st && !stv)
        RED_ONCE(104, "stencil-mode texture view of Metal format %lu (texture ID %d): no stencil-only view, depth is read", (unsigned long)base.pixelFormat, tid);
    if ((r.has_v && (r.v.set_format || r.v.set_levels || r.v.set_layers || r.v.set_target)) || need_sw || stv) {
        if (!r.has_v) memset(&r.v, 0, sizeof r.v);
        uint32_t pf = (uint32_t)base.pixelFormat; uint8_t swz[4];
        if (r.v.set_format && !(nvn_fmt_to_mtl(r.v.format, &pf, swz) && pf)) pf = (uint32_t)base.pixelFormat;
        { uint32_t upf; int ubpp, kind;
          if (r.v.set_format && is_astc4(base.pixelFormat) && nvn_bc_astc_fmt((uint32_t)r.v.format, &upf, &kind)) pf = upf;
          else if (r.v.set_format && bc_unpack(r.v.format, &upf, &ubpp)) pf = upf; }
        if (stv) pf = MTLPixelFormatX32_Stencil8;
        if (r.v.set_format && nvn_fmt_bc(rp->t.d.format) && !nvn_fmt_bc(r.v.format)) {   // the raw blocks of a BC texture through an uncompressed view (see rt_view_fmt): Metal has no such view, with or without BC
            RED_ONCE(86, "压缩格式贴图的原始块视图（NVN 格式 %#x → 视图 %#x）Metal 建不出：按空槽", rp->t.d.format, r.v.format);
            return nil;
        }
        if (tex_need_pfv(base.pixelFormat, (MTLPixelFormat)pf) && !(base.usage & MTLTextureUsagePixelFormatView)) {
            base = tex_upgrade_pfv(&rp->t, base, (MTLPixelFormat)pf);
            sg = g_ser_gen;
        }
        NSUInteger l0 = r.v.set_levels ? r.v.base_level : 0, nl = r.v.set_levels && r.v.nlevels ? r.v.nlevels : base.mipmapLevelCount - l0;
        NSUInteger s0 = r.v.set_layers ? r.v.min_layer : 0;
        NSUInteger ns = base.textureType == MTLTextureType3D ? 1 : (r.v.set_layers && r.v.nlayers ? r.v.nlayers : base.arrayLength * (base.textureType == MTLTextureTypeCube || base.textureType == MTLTextureTypeCubeArray ? 6 : 1) - s0);
        MTLTextureType tt = r.v.set_target ? ttype(r.v.target, base.textureType) : base.textureType;
        if (tt == MTLTextureType2D && ns > 1) ns = 1;
        v = need_sw ? [base newTextureViewWithPixelFormat:(MTLPixelFormat)pf textureType:tt levels:NSMakeRange(l0, nl) slices:NSMakeRange(s0, ns) swizzle:MTLTextureSwizzleChannelsMake(sw[0], sw[1], sw[2], sw[3])]
                    : [base newTextureViewWithPixelFormat:(MTLPixelFormat)pf textureType:tt levels:NSMakeRange(l0, nl) slices:NSMakeRange(s0, ns)];
        if (!v) { RED_ONCE(11, "贴图视图建不出 id %d", tid); v = base; }
    }
    rp->view = (void *)CFBridgingRetain(v); rp->view_rid = legacy_texture_rid(v); rp->view_ser = sg; rp->view_dirty = dg;
    return v;
}
// Hot-path object fetch: if the table entry is valid, return the table-held pointer directly (without ARC retain / autorelease), otherwise take the slow path to build / validate. Caller holds g_rmu.
static inline __attribute__((always_inline)) void *tex_fast(int tid) {
    if (tid > 0 && tid < g_ntreg && g_treg[tid].ok && g_treg[tid].view && g_treg[tid].view_ser == g_ser_gen && (!g_treg[tid].view_pkg || g_treg[tid].view_dirty == nvn_dirty_now())) return g_treg[tid].view;
    return (__bridge void *)tex_by_id(tid);
}
// Some Z-A descriptors expose one ordinary 2D texture even though the translated sampling
// instruction is a cube read. Metal cannot make a cube view from a standalone 2D allocation;
// binding it anyway returns undefined (black on Apple M2). Promote the image to a six-slice
// private allocation on a separate, earlier-committed command buffer, then sample its cube
// view. Square images are replicated to every face. The two common six-face strip layouts
// and the 4x3 / 3x4 cross layouts are split into their individual faces.
//
// The copy command buffer is committed while the current render command buffer is still being
// encoded. Metal executes committed buffers in queue order, so the promotion finishes before
// the current draw without ending its render pass. A packaged upload is flushed first for the
// same reason. The alternate view cache is invalidated automatically when the source MTLTexture
// object changes after a guest rewrite.
static id<MTLTexture> tex_promote_2d_cube(int tid, id<MTLTexture> t, uint64_t *rid) {
    if (!t || t.textureType != MTLTextureType2D || t.sampleCount != 1 || !t.width || !t.height) return nil;

    NSUInteger face = MIN(t.width, t.height), layout = 0;
    if (t.width >= t.height * 6) { face = t.height; layout = 1; }
    else if (t.height >= t.width * 6) { face = t.width; layout = 2; }
    else if (t.width * 3 == t.height * 4) { face = t.width / 4; layout = 3; }
    else if (t.width * 4 == t.height * 3) { face = t.width / 3; layout = 4; }
    if (!face) return nil;

    NSUInteger levels = MIN(t.mipmapLevelCount, (NSUInteger)(64 - __builtin_clzll(face)));
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = MTLTextureType2DArray; d.pixelFormat = t.pixelFormat;
    d.width = face; d.height = face; d.depth = 1; d.arrayLength = 6;
    d.mipmapLevelCount = MAX(levels, 1u); d.sampleCount = 1;
    d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> base = [g_dev newTextureWithDescriptor:d];
    if (!base) return nil;
    res_add(base);

    up_flush();
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    cb.label = [NSString stringWithFormat:@"promote tid %d 2D to cube", tid];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    static const uint8_t cross4x3[6][2] = {{2,1},{0,1},{1,0},{1,2},{1,1},{3,1}};
    static const uint8_t cross3x4[6][2] = {{2,1},{0,1},{1,0},{1,2},{1,1},{1,3}};
    for (NSUInteger lv = 0; lv < d.mipmapLevelCount; lv++) {
        NSUInteger f = MAX(face >> lv, 1u);
        for (NSUInteger sl = 0; sl < 6; sl++) {
            NSUInteger x = 0, y = 0;
            if (layout == 1) x = sl * f;
            else if (layout == 2) y = sl * f;
            else if (layout == 3) { x = cross4x3[sl][0] * f; y = cross4x3[sl][1] * f; }
            else if (layout == 4) { x = cross3x4[sl][0] * f; y = cross3x4[sl][1] * f; }
            NSUInteger sw = MAX(t.width >> lv, 1u), sh = MAX(t.height >> lv, 1u);
            if (x + f > sw || y + f > sh) { x = y = 0; f = MIN(f, MIN(sw, sh)); }
            [b copyFromTexture:t sourceSlice:0 sourceLevel:lv sourceOrigin:MTLOriginMake(x, y, 0)
                   sourceSize:MTLSizeMake(f, f, 1) toTexture:base destinationSlice:sl destinationLevel:lv destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
    }
    [b endEncoding];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) { if (done.error) MLOG("【红】2D→Cube 提升拷贝失败（贴图号 %d）：%s", tid, done.error.description.UTF8String); }];
    res_commit(); [cb commit];

    id<MTLTexture> v = [base newTextureViewWithPixelFormat:t.pixelFormat textureType:MTLTextureTypeCube
                                                     levels:NSMakeRange(0, d.mipmapLevelCount) slices:NSMakeRange(0, 6) swizzle:t.swizzle];
    if (!v) { res_remove(base); return nil; }
    treg_t *r = &g_treg[tid];
    r->alt = (void *)CFBridgingRetain(v); r->alt_of = (__bridge void *)t;
    r->alt_type = (uint8_t)MTLTextureTypeCube; r->alt_rid = legacy_texture_rid(v);
    *rid = r->alt_rid;
    static uint64_t promoted;
    MLOG("独立 2D 贴图提升为 Cube：tid=%d ser=%llu %lux%lu fmt=%lu 布局=%lu（第 %llu 张）", tid,
         tid > 0 && tid < g_ntreg ? (unsigned long long)g_treg[tid].t.serial : 0ull,
         (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat,
         (unsigned long)layout, (unsigned long long)++promoted);
    return v;
}
// The texture type the shader declares differs from the one the descriptor gives: use a compatible view. Maxwell sample instructions carry their own dimension, and a non-array instruction reading a 2D array texture reads layer 0;
// on Metal a 2D array bound to a texture2d argument is a type mismatch with undefined reads (measured: one fixed value). Super Mario 3D World + Bowser's Fury creates its pre-lighting buffer as a one-layer 2D array
// and the deferred lighting shader samples it as 2D: the ambient light of the whole picture became one colour, washed out white. Supports 2D array <-> 2D and 2D array -> cube / cube array; any other mismatch is reported once. Caller holds g_rmu
static id<MTLTexture> tex_fit(int tid, id<MTLTexture> t, MTLTextureType want, uint64_t *rid) {
    MTLTextureType have = t.textureType; NSUInteger ns = 0, l0 = 0, nl = t.mipmapLevelCount, s0 = 0; id<MTLTexture> src = t;
    int cube_parent = t.parentTexture && t.parentTexture.textureType == MTLTextureType2DArray &&
                      t.parentRelativeSlice + 6 <= t.parentTexture.arrayLength;
    if (want == MTLTextureTypeCube && have == MTLTextureType2D && !cube_parent && tid > 0 && tid < g_ntreg) {
        treg_t *r = &g_treg[tid];
        if (r->alt && r->alt_of == (__bridge void *)t && r->alt_type == (uint8_t)want) { *rid = r->alt_rid; return (__bridge id<MTLTexture>)r->alt; }
        if (r->alt) { defer_obj_unref((__bridge id)r->alt); CFRelease(r->alt); r->alt = r->alt_of = NULL; r->alt_rid = 0; }
        id<MTLTexture> promoted = tex_promote_2d_cube(tid, t, rid);
        if (promoted) return promoted;
    }
    if (want == MTLTextureType2D && have == MTLTextureType2DArray) ns = 1;
    else if (want == MTLTextureType2DArray && have == MTLTextureType2D) ns = 1;
    else if (want == MTLTextureTypeCube && have == MTLTextureType2DArray && t.arrayLength >= 6) ns = 6;
    else if (want == MTLTextureTypeCubeArray && have == MTLTextureType2DArray && t.arrayLength >= 6) ns = t.arrayLength / 6 * 6;
    else if (want == MTLTextureTypeCube && have == MTLTextureTypeCubeArray) ns = 6;   // a cube array read as a cube: the first cube
    else if (want == MTLTextureTypeCube && have == MTLTextureType2D && t.parentTexture && t.parentTexture.textureType == MTLTextureType2DArray &&
             t.parentRelativeSlice + 6 <= t.parentTexture.arrayLength) {   // A descriptor view narrowed a cube's 2D-array base to one 2D slice, but the sampling instruction still declares cube
        src = t.parentTexture; ns = 6; l0 = t.parentRelativeLevel; nl = t.mipmapLevelCount; s0 = t.parentRelativeSlice;
    }
    if (!ns || tid <= 0 || tid >= g_ntreg) { RED_ONCE(76, "贴图类型不符：着色器声明 %lu，贴图 %lu（贴图号 %d），照原样绑", (unsigned long)want, (unsigned long)have, tid); return nil; }
    treg_t *r = &g_treg[tid];
    if (r->alt && r->alt_of == (__bridge void *)t && r->alt_type == (uint8_t)want) { *rid = r->alt_rid; return (__bridge id<MTLTexture>)r->alt; }
    if (r->alt) { defer_obj_unref((__bridge id)r->alt); CFRelease(r->alt); r->alt = r->alt_of = NULL; r->alt_rid = 0; }
    id<MTLTexture> v = [src newTextureViewWithPixelFormat:t.pixelFormat textureType:want levels:NSMakeRange(l0, nl) slices:NSMakeRange(s0, ns) swizzle:t.swizzle];
    if (!v) { RED_ONCE(77, "贴图类型兼容视图建不出：%lu → %lu（贴图号 %d）", (unsigned long)have, (unsigned long)want, tid); return nil; }
    r->alt = (void *)CFBridgingRetain(v); r->alt_of = (__bridge void *)t; r->alt_type = (uint8_t)want; r->alt_rid = legacy_texture_rid(v);
    *rid = r->alt_rid; return v;
}
// Alpha from red: the fragment shaders the game profile's alpha_from_red lists read a single-channel render target whose channel mapping is exactly (R, 0, 0, 1) as (R, 0, 0, R).
// A workaround for NintendoWare layout captures (BotW, Odyssey): the game rebuilds each capture on the same storage with its own mapping ((1, 1, 1, R) for an A8 capture), which was lost while
// nvnTextureCompare ignored the mapping and left the creation-time (R, 0, 0, 1), alpha always 1 (hard-edged prompt boxes, white coin cards). Since that fix it no longer fires (docs/agents/04-graphics.md).
// Only render targets qualify (captures always are; asset and CPU-written textures are not). DM_NVN_ALPHA_RED=0 turns it off, <keys,...> replaces the list. Caller holds g_rmu
static char g_afr_keys[48][20]; static int g_afr_nk = -1;
static int afr_want(NvShader *sh) {
    if (!sh->afr) {
        if (g_afr_nk < 0) { g_afr_nk = 0; const char *e = getenv("DM_NVN_ALPHA_RED"), *src = e && *e ? e : "";
            if (strcmp(src, "0")) for (const char *q = src; *q && g_afr_nk < 48;) { size_t n = strcspn(q, ", "); if (n) { if (n > 16) n = 16; memcpy(g_afr_keys[g_afr_nk], q, n); g_afr_keys[g_afr_nk++][n] = 0; } q += n; q += strspn(q, ", "); }
            if (g_afr_nk) MLOG("单通道贴图 alpha 取红：%s%s", src, e && *e ? "（DM_NVN_ALPHA_RED）" : "（游戏档案 alpha_from_red）"); }
        sh->afr = 1; for (int i = 0; i < g_afr_nk; i++) if (!strncmp(sh->key, g_afr_keys[i], strlen(g_afr_keys[i]))) sh->afr = 2;
    }
    return sh->afr == 2;
}
static id<MTLTexture> tex_alpha_red(int tid, id<MTLTexture> t, uint64_t *rid) {
    MTLPixelFormat pf = t.pixelFormat; MTLTextureSwizzleChannels sw = t.swizzle;
    if (pf != MTLPixelFormatR8Unorm && pf != MTLPixelFormatR16Unorm && pf != MTLPixelFormatR16Float && pf != MTLPixelFormatR32Float) return nil;
    if (sw.red != MTLTextureSwizzleRed || sw.green != MTLTextureSwizzleZero || sw.blue != MTLTextureSwizzleZero || sw.alpha != MTLTextureSwizzleOne || tid <= 0 || tid >= g_ntreg) return nil;
    id<MTLTexture> base = t.parentTexture ? t.parentTexture : t;
    if (!(base.usage & MTLTextureUsageRenderTarget)) return nil;   // packaged and CPU-written textures are built without render-target usage
    treg_t *r = &g_treg[tid];
    if (r->ar && r->ar_of == (__bridge void *)t) { *rid = r->ar_rid; return (__bridge id<MTLTexture>)r->ar; }
    treg_drop_ar(r);
    NSUInteger l0 = t.parentTexture ? t.parentRelativeLevel : 0, s0 = t.parentTexture ? t.parentRelativeSlice : 0;
    NSUInteger ns = t.textureType == MTLTextureType3D ? 1 : t.arrayLength * (t.textureType == MTLTextureTypeCube || t.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    id<MTLTexture> v = [base newTextureViewWithPixelFormat:pf textureType:t.textureType levels:NSMakeRange(l0, t.mipmapLevelCount) slices:NSMakeRange(s0, ns)
                                                   swizzle:MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleZero, MTLTextureSwizzleZero, MTLTextureSwizzleRed)];
    if (!v) { RED_ONCE(82, "alpha 取红的视图建不出（贴图号 %d 格式 %lu）", tid, (unsigned long)pf); return nil; }
    { static int once; if (!once++) MLOG("单通道贴图 alpha 取红：首例 贴图号 %d %lux%lu", tid, (unsigned long)t.width, (unsigned long)t.height); }
    r->ar = (void *)CFBridgingRetain(v); r->ar_of = (__bridge void *)t; r->ar_rid = legacy_texture_rid(v);
    *rid = r->ar_rid; return v;
}
static inline __attribute__((always_inline)) uint64_t tex_fast_rid(int tid, void **out) {
    if (tid > 0 && tid < g_ntreg && g_treg[tid].ok && g_treg[tid].view && g_treg[tid].view_ser == g_ser_gen && (!g_treg[tid].view_pkg || g_treg[tid].view_dirty == nvn_dirty_now())) { if (out) *out = g_treg[tid].view; return g_treg[tid].view_rid; }
    id<MTLTexture> t = tex_by_id(tid); if (out) *out = (__bridge void *)t; return t ? legacy_texture_rid(t) : 0;
}
static MTLSamplerAddressMode wrapm(int w) {
    switch (w) { case 0: case 7: return MTLSamplerAddressModeClampToEdge; case 1: return MTLSamplerAddressModeRepeat;
        case 3: case 2: return MTLSamplerAddressModeMirrorClampToEdge; case 5: case 4: return MTLSamplerAddressModeClampToBorderColor;
        case 6: return MTLSamplerAddressModeMirrorRepeat; }
    return MTLSamplerAddressModeRepeat;
}
static id<MTLSamplerState> samp_by_id(int sid) {   // Caller holds g_rmu
    if (sid < 0 || sid >= g_nsreg || !g_sreg_ok[sid]) { RED_ONCE(12, "采样器号 %d 未登记", sid); return nil; }
    if (g_sview[sid]) return (__bridge id<MTLSamplerState>)g_sview[sid];
    samp_par p = g_sreg[sid];
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.magFilter = p.magf ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.minFilter = (p.minf == 1 || p.minf == 3 || p.minf == 5) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = p.minf <= 1 ? MTLSamplerMipFilterNotMipmapped : (p.minf == 2 || p.minf == 3) ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    d.sAddressMode = wrapm(p.ws); d.tAddressMode = wrapm(p.wt); d.rAddressMode = wrapm(p.wr);
    d.lodMinClamp = p.lod_min < 0 ? 0 : p.lod_min; d.lodMaxClamp = p.lod_max;
    d.maxAnisotropy = p.aniso < 1 ? 1 : p.aniso > 16 ? 16 : (NSUInteger)p.aniso;
    if (p.cmp_mode) d.compareFunction = (MTLCompareFunction)(p.cmp_func - 1);
    // Public on Metal 4 / iOS 26. Dynamic lookup keeps the same binary
    // launchable on iOS 16, whose descriptor has no reduction-mode selector.
    if (p.reduction == 1 || p.reduction == 2) {
        SEL setReduction = NSSelectorFromString(@"setReductionMode:");
        if ([d respondsToSelector:setReduction]) {
            [d setValue:@(p.reduction == 1 ? 1 : 2) forKey:@"reductionMode"];
            if (d.mipFilter == MTLSamplerMipFilterNotMipmapped) { d.lodMinClamp = 0; d.lodMaxClamp = 0; }
            d.mipFilter = MTLSamplerMipFilterLinear;
        }
    }
    d.borderColor = p.border[3] > 0.5f ? (p.border[0] > 0.5f ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorOpaqueBlack) : MTLSamplerBorderColorTransparentBlack;
    d.supportArgumentBuffers = YES;
    id<MTLSamplerState> s = [g_dev newSamplerStateWithDescriptor:d];
    if (s) { g_sview[sid] = (void *)CFBridgingRetain(s); g_srid[sid] = legacy_sampler_rid(s); }
    return s;
}
static inline __attribute__((always_inline)) void *samp_fast(int sid) {
    if (sid > 0 && sid < g_nsreg && g_sreg_ok[sid] && g_sview[sid]) return g_sview[sid];
    return (__bridge void *)samp_by_id(sid);
}

// ---------- Argument buffer blocks: one block per submission, reclaimed after the GPU completes ----------
@interface NvArena : NSObject { @public id<MTLBuffer> buf; uint64_t gpu, fence; size_t used, len; } @end
@implementation NvArena @end
static NSMutableArray<NvArena *> *g_arenas; static NvArena *g_cur_arena; static NSMutableArray<NvArena *> *g_sub_arenas;
static NvArena *arena_new(void) {
    // The for-in loop variable does not retain the object: take a strong reference first and then remove it from the array, otherwise what is returned after the array releases it is a dangling pointer
    for (NvArena *it in g_arenas) if (it->fence && it->fence <= g_done) { NvArena *r = it; [g_arenas removeObject:r]; r->fence = 0; r->used = 0; return r; }
    NvArena *a = [NvArena new]; a->buf = [g_dev newBufferWithLength:4 << 20 options:MTLResourceStorageModeShared]; a->len = a->buf.length; a->gpu = legacy_buffer_gpu(a->buf); res_add(a->buf); g_arena_made++; return a;
}
void arena_begin(void) { if (!g_arenas) { g_arenas = [NSMutableArray array]; g_sub_arenas = [NSMutableArray array]; } g_cur_arena = nil; }
void arena_commit(uint64_t fence) { for (NvArena *a in g_sub_arenas) { a->fence = fence; [g_arenas addObject:a]; } [g_sub_arenas removeAllObjects]; g_cur_arena = nil; }
// The out parameter is strong: under ARC an unqualified id * is autoreleasing,
// which otherwise retains the arena once per allocation until the thread exits.
static size_t arena_alloc(size_t n, __strong id<MTLBuffer> *buf) {
    n = (n + 255) & ~(size_t)255;
    if (!g_cur_arena || g_cur_arena->used + n > g_cur_arena->len) {
        pthread_mutex_lock(&g_mu); g_cur_arena = arena_new(); pthread_mutex_unlock(&g_mu);
        [g_sub_arenas addObject:g_cur_arena];
    }
    size_t off = g_cur_arena->used; g_cur_arena->used += n; *buf = g_cur_arena->buf; return off;
}

// ---------- Draw state ----------
static struct {
    const prog_t *prog;
    const prog_t *bst[5];      // program bound per stage (NVN stage bits 1 vertex 2 fragment 4 geometry 8 / 16 tessellation, indices as in NvProg.sh), see bind_stages
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[16]; int nattr; vs_st stream[16]; int nstream;
    NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t tex[6][NTEX], samp[6][128], img[6][32];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][4]; float dr[2]; int has_vp, depth_clamp;
    float blend_color[4]; int64_t copy_stride, copy_img_stride;
    p_tf_bind tf[4]; uint64_t tf_off[4]; int tf_on, rast_discard;
} S;

// Size of an attachment-less pass: the far corner of viewport 0, otherwise of
// the scissor, clamped to Metal's supported render-target extent.
static void bare_dims(NSUInteger *w, NSUInteger *h) {
    double x1 = S.has_vp ? MAX(S.vp[0][0], S.vp[0][0] + S.vp[0][2]) : R.has_sc ? (double)R.sc.x + R.sc.w : 1;
    double y1 = S.has_vp ? MAX(S.vp[0][1], S.vp[0][1] + S.vp[0][3]) : R.has_sc ? (double)R.sc.y + R.sc.h : 1;
    *w = (NSUInteger)MIN(MAX(ceil(x1), 1.0), 16384.0);
    *h = (NSUInteger)MIN(MAX(ceil(y1), 1.0), 16384.0);
}

// Adapted from upstream 89b608d: cache resolution separately from encoder state.
// A pool replacement invalidates all entries even if guest addresses are reused.
static void bind_vbufs(id<MTLRenderCommandEncoder> en) {
    static struct { uint64_t addr, gen; NSUInteger offset; __unsafe_unretained id<MTLBuffer> buffer; } cache[16];
    uint64_t gen = atomic_load_explicit(&g_pool_gen, memory_order_acquire);
    for (int s = 0; s < 16; s++) {
        uint64_t addr = S.vbuf[s].address;
        if (!addr) continue;
        if (g_vbuf_cache && cache[s].addr == addr && cache[s].gen == gen) g_vbuf_reuse++;
        else {
            void *mb; uint64_t off; g_vbuf_resolve++;
            if (!mtl_addr(addr, 1, &mb, &off)) continue;
            cache[s].addr = addr; cache[s].gen = gen;
            cache[s].buffer = (__bridge id<MTLBuffer>)mb; cache[s].offset = (NSUInteger)off;
        }
        legacy_bind_buffer(en, 0, VB_SLOT(s), cache[s].buffer, cache[s].offset);
        use_rec("buf", addr, S.vbuf[s].size, 0, NULL);
    }
    static id<MTLBuffer> zero;
    if (!zero) { zero = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; res_add(zero); }
    legacy_bind_buffer(en, 0, 30, zero, 0);
}
static void udc_note_draw(NvProg *pr) {
    if (!tbdr_on()) {   // without the TBDR set only the duplicate-slot report below is live (the zero-source skip and the unwritten-attachment store are part of the set): skip the per-draw scans
        uint32_t cm = pr && pr->sh[1] ? pr->sh[1]->rf.color_mask : 0;
        if (R.has_rt && cm >> 1) for (int i = 1; i < R.rt.n && i < 8; i++)
            if ((cm >> i & 1) && ((S.chmask.mask >> (4 * i)) & 0xf) && rt_dup_slot(&R.rt, i))
                RED_ONCE(79, "颜色槽 %d 与前面的槽挂同一张贴图（ser=%llu）且本次绘制写它（掩码 %#x）：这些写入丢了", i, (unsigned long long)R.rt.color[i]->serial, (S.chmask.mask >> (4 * i)) & 0xf);
        return;
    }
    if (R.has_rt) for (int i = 0; i < R.rt.n && i < 8; i++) if (R.rt.color[i]) { zskip_del(R.rt.color[i]->serial); g_zskip_cand_ok[i] = 0; }   // Zero-source set: once really drawn it is no longer all zeros
    if (pr) for (int st = 0; st < 6; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh) continue; for (int i = 0; i < sh->rf.nb; i++) if (sh->rf.b[i].kind == NVN_RB_IMG) { zskip_clear(); break; } }   // Image slots are writable: conservatively clear
    int image_write = 0;
    uint32_t shader_color_mask = 0;
    if (pr) for (int st = 0; st < 6; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh) continue;
        if (st == 1) shader_color_mask = sh->rf.color_mask;
        for (int i = 0; i < sh->rf.nb; i++) if (sh->rf.b[i].kind == NVN_RB_IMG && sh->rf.b[i].rw) { image_write = 1; break; }
        if (image_write) break;
    }
    if (R.has_rt) for (int i = 1; i < R.rt.n && i < 8; i++)   // a slot attached twice (left unattached) really written: the writes are lost (see rt_dup_slot)
        if ((shader_color_mask >> i & 1) && ((S.chmask.mask >> (4 * i)) & 0xf) && rt_dup_slot(&R.rt, i))
            RED_ONCE(79, "颜色槽 %d 与前面的槽挂同一张贴图（ser=%llu）且本次绘制写它（掩码 %#x）：这些写入丢了", i, (unsigned long long)R.rt.color[i]->serial, (S.chmask.mask >> (4 * i)) & 0xf);
    for (int i = 0; i < g_nsd; i++) {
        sd_att *t = &g_sd[i];
        if (image_write) { t->wrote = 1; t->wwhy = 1; }
        else if (t->depth == 0 && t->idx >= 0 && (((S.chmask.mask >> (4 * t->idx)) & 0xf) || (shader_color_mask >> t->idx & 1))) { t->wrote = 1; t->wwhy = (S.chmask.mask >> (4 * t->idx)) & 0xf ? 2 : 3; }
        else if (t->depth == 1 && S.ds.dw) { t->wrote = 1; t->wwhy = 4; }
        else if (t->depth == 2 && S.ds.st && (S.st_wmask[0] || S.st_wmask[1])
                && (S.ds.fail_f != NVN_STENCIL_OP_KEEP || S.ds.zfail_f != NVN_STENCIL_OP_KEEP || S.ds.zpass_f != NVN_STENCIL_OP_KEEP
                    || S.ds.fail_b != NVN_STENCIL_OP_KEEP || S.ds.zfail_b != NVN_STENCIL_OP_KEEP || S.ds.zpass_b != NVN_STENCIL_OP_KEEP)) { t->wrote = 1; t->wwhy = 5; }
    }
}
// NVN's BindProgram(program, stage bits) binds per stage: the stages in the bits take that program's shaders (a null program unbinds them), the other stages stay.
// Metroid Dread's depth prepass calls BindProgram(program, vertex | fragment), then BindProgram(null, fragment): only the vertex shader remains and depth is drawn.
// Every bind used to replace S.prog as a whole, a null program meant "no program bound", and the prepass's ~140 draws per frame were all dropped: depth stayed empty, the G-buffer's depth EQUAL test passed no pixel, and scenes and characters were black.
// When every stage comes from the same program, that program is the effective one (all of Mario's and Engage's binds are like this, so nothing changes for them); otherwise a composite is made: a host-made prog_t carrying only a serial, whose NvProg in the serial table takes each stage from its source program (a null fragment = a vertex-only pipeline)
static const prog_t *bind_stages(const prog_t *p, int stages) {
    for (int st = 0; st < 5; st++) if (stages >> st & 1) S.bst[st] = p;
    if (stages & 0x20) return p;   // compute: replaced as a whole as before
    const prog_t *v = S.bst[0];
    if (!v) return NULL;
    int same = 1; for (int st = 1; st < 5; st++) if (S.bst[st] && S.bst[st] != v) same = 0;
    if (same && S.bst[1] == v) return v;
    enum { NCMP = 64 };
    static prog_t cp[NCMP]; static const prog_t *ck[NCMP][5]; static uint64_t cs[NCMP][5]; static int ncp, next;
    for (int i = 0; i < ncp; i++) {
        int hit = 1; for (int st = 0; st < 5 && hit; st++) hit = ck[i][st] == S.bst[st] && (!S.bst[st] || cs[i][st] == S.bst[st]->serial);
        if (hit) return &cp[i];
    }
    int i = ncp < NCMP ? ncp++ : next++ % NCMP;
    if (cp[i].serial) serial_del(cp[i].serial);
    NvProg *o = [NvProg new];
    for (int st = 0; st < 5; st++) { ck[i][st] = S.bst[st]; cs[i][st] = S.bst[st] ? S.bst[st]->serial : 0; if (S.bst[st]) { __unsafe_unretained NvProg *src = prog_get(S.bst[st]); o->sh[st] = src ? src->sh[st] : nil; } }
    memset(&cp[i], 0, sizeof cp[i]); cp[i].magic = MAGIC('P', 'R', 'O', 'G'); cp[i].serial = nvn_new_serial(); serial_put(cp[i].serial, o);
    { static int once; if (!once++) MLOG("按阶段绑程序：首个合成程序（顶点 %s 片元 %s）", o->sh[0] ? o->sh[0]->key : "-", o->sh[1] ? o->sh[1]->key : "-"); }
    return &cp[i];
}
static void state_reset(void) {
    memset(&S, 0, sizeof S);
    g_pso_dirty = g_dss_dirty = 1;
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; S.chmask.mask = 0xffffffffu;
    S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    S.st_vmask[0] = S.st_vmask[1] = S.st_wmask[0] = S.st_wmask[1] = 0xff; S.dr[1] = 1;
}

// Correct the vertex format by the shader input's component type: Metal requires integer inputs to get integer formats of the same signedness, and float inputs float / normalized formats.
// Integer input (inferred at translation time as "only used as an integer", dmsh_build.py int_inputs) with an integer format: on a signedness mismatch switch to the same width with the other sign (bit pattern unchanged); with a 32-bit float format read it as an integer of the same width (bit pattern unchanged).
// Other mismatches (integer input with a normalized / half format, float input with an integer format) cannot keep the bit pattern: report once and keep the format.
static MTLVertexFormat vfmt_for_ct(MTLVertexFormat f, int ct, int nvn_fmt) {
    static const MTLVertexFormat U[] = { MTLVertexFormatUChar, MTLVertexFormatUChar2, MTLVertexFormatUChar3, MTLVertexFormatUChar4, MTLVertexFormatUShort, MTLVertexFormatUShort2, MTLVertexFormatUShort3, MTLVertexFormatUShort4, MTLVertexFormatUInt, MTLVertexFormatUInt2, MTLVertexFormatUInt3, MTLVertexFormatUInt4 };
    static const MTLVertexFormat I[] = { MTLVertexFormatChar, MTLVertexFormatChar2, MTLVertexFormatChar3, MTLVertexFormatChar4, MTLVertexFormatShort, MTLVertexFormatShort2, MTLVertexFormatShort3, MTLVertexFormatShort4, MTLVertexFormatInt, MTLVertexFormatInt2, MTLVertexFormatInt3, MTLVertexFormatInt4 };
    static const MTLVertexFormat F[] = { MTLVertexFormatFloat, MTLVertexFormatFloat2, MTLVertexFormatFloat3, MTLVertexFormatFloat4 };
    int iu = -1, ii = -1, iff = -1;
    for (int i = 0; i < 12; i++) { if (U[i] == f) iu = i; if (I[i] == f) ii = i; }
    for (int i = 0; i < 4; i++) if (F[i] == f) iff = i;
    if (ct == NVN_CT_UINT) { if (iu >= 0) return f; if (ii >= 0) return U[ii]; if (iff >= 0) return U[8 + iff]; }
    else if (ct == NVN_CT_INT) { if (ii >= 0) return f; if (iu >= 0) return I[iu]; if (iff >= 0) return I[8 + iff]; }
    else { if (iu < 0 && ii < 0) return f; }
    RED_ONCE(66, "顶点属性格式 %#x 与着色器输入类型 %s 对不上，位模式保不住（照原格式）", nvn_fmt, ct == NVN_CT_UINT ? "uint" : ct == NVN_CT_INT ? "int" : "float");
    return f;
}
static MTLVertexFormat vfmt(int f) {
    switch (f) {
        case 0x01: return MTLVertexFormatUCharNormalized; case 0x02: return MTLVertexFormatCharNormalized; case 0x03: return MTLVertexFormatUChar; case 0x04: return MTLVertexFormatChar;
        case 0x05: return MTLVertexFormatHalf; case 0x06: return MTLVertexFormatUShortNormalized; case 0x07: return MTLVertexFormatShortNormalized;
        case 0x08: return MTLVertexFormatUShort; case 0x09: return MTLVertexFormatShort; case 0x0A: return MTLVertexFormatFloat; case 0x0B: return MTLVertexFormatUInt; case 0x0C: return MTLVertexFormatInt;
        case 0x0D: return MTLVertexFormatUChar2Normalized; case 0x0E: return MTLVertexFormatChar2Normalized; case 0x0F: return MTLVertexFormatUChar2; case 0x10: return MTLVertexFormatChar2;
        case 0x11: return MTLVertexFormatHalf2; case 0x12: return MTLVertexFormatUShort2Normalized; case 0x13: return MTLVertexFormatShort2Normalized; case 0x14: return MTLVertexFormatUShort2;
        case 0x15: return MTLVertexFormatShort2; case 0x16: return MTLVertexFormatFloat2; case 0x17: return MTLVertexFormatUInt2; case 0x18: return MTLVertexFormatInt2;
        case 0x19: return MTLVertexFormatUChar3Normalized; case 0x1A: return MTLVertexFormatChar3Normalized; case 0x1B: return MTLVertexFormatUChar3; case 0x1C: return MTLVertexFormatChar3;
        case 0x1D: return MTLVertexFormatHalf3; case 0x1E: return MTLVertexFormatUShort3Normalized; case 0x1F: return MTLVertexFormatShort3Normalized; case 0x20: return MTLVertexFormatUShort3;
        case 0x21: return MTLVertexFormatShort3; case 0x22: return MTLVertexFormatFloat3; case 0x23: return MTLVertexFormatUInt3; case 0x24: return MTLVertexFormatInt3;
        case 0x25: return MTLVertexFormatUChar4Normalized; case 0x26: return MTLVertexFormatChar4Normalized; case 0x27: return MTLVertexFormatUChar4; case 0x28: return MTLVertexFormatChar4;
        case 0x29: return MTLVertexFormatHalf4; case 0x2A: return MTLVertexFormatUShort4Normalized; case 0x2B: return MTLVertexFormatShort4Normalized; case 0x2C: return MTLVertexFormatUShort4;
        case 0x2D: return MTLVertexFormatShort4; case 0x2E: return MTLVertexFormatFloat4; case 0x2F: return MTLVertexFormatUInt4; case 0x30: return MTLVertexFormatInt4;
        case 0x3D: return MTLVertexFormatUInt1010102Normalized; case 0x69: return MTLVertexFormatInt1010102Normalized; case 0x3F: return MTLVertexFormatInvalid; case 0x9A: return MTLVertexFormatUChar4Normalized_BGRA;
    }
    return MTLVertexFormatInvalid;
}
static MTLBlendFactor bfac(int f) {
    switch (f) { case 1: return MTLBlendFactorZero; case 2: return MTLBlendFactorOne; case 3: return MTLBlendFactorSourceColor; case 4: return MTLBlendFactorOneMinusSourceColor;
        case 5: return MTLBlendFactorSourceAlpha; case 6: return MTLBlendFactorOneMinusSourceAlpha; case 7: return MTLBlendFactorDestinationAlpha; case 8: return MTLBlendFactorOneMinusDestinationAlpha;
        case 9: return MTLBlendFactorDestinationColor; case 10: return MTLBlendFactorOneMinusDestinationColor; case 11: return MTLBlendFactorSourceAlphaSaturated;
        case 0x10: return MTLBlendFactorSource1Color; case 0x11: return MTLBlendFactorOneMinusSource1Color; case 0x12: return MTLBlendFactorSource1Alpha; case 0x13: return MTLBlendFactorOneMinusSource1Alpha;
        case 0x61: return MTLBlendFactorBlendColor; case 0x62: return MTLBlendFactorOneMinusBlendColor; case 0x63: return MTLBlendFactorBlendAlpha; case 0x64: return MTLBlendFactorOneMinusBlendAlpha; }
    return MTLBlendFactorOne;
}
static MTLBlendOperation bop(int e) { switch (e) { case 2: return MTLBlendOperationSubtract; case 3: return MTLBlendOperationReverseSubtract; case 4: return MTLBlendOperationMin; case 5: return MTLBlendOperationMax; } return MTLBlendOperationAdd; }

// Pipeline: cached by (program, target formats, blend / write mask, vertex layout, sample count)
static const uint8_t *g_pso_lmap; static id<MTLFunction> g_pso_fsfn; static pso_fz_desc g_pso_fz, g_fz_desc;
static os_unfair_lock g_fz_fn_lock = OS_UNFAIR_LOCK_INIT;
static NSMutableDictionary<NSString *, id<MTLFunction>> *g_bf_fn_cache;
static NSMutableDictionary<NSString *, id<MTLLibrary>> *g_bf_lib_cache;
static nvn_hmap g_pso = {.klen = sizeof(pso_key)}, g_dss;
static nvn_hmap g_pso_fail = {.klen = sizeof(pso_key)};   // pipelines that failed to build: remembered and never retried (previously every draw recompiled it, over 10,000 times in one iPhone session)
static const char g_tf_mark;
#define TF_MARK ((const void *)&g_tf_mark)
static uint8_t g_pso_topology = MTLPrimitiveTopologyClassTriangle;
static uint8_t pso_topology_class(int prim) {
    if (prim == 0) return MTLPrimitiveTopologyClassPoint;
    if (prim == 1 || prim == 3) return MTLPrimitiveTopologyClassLine;
    return MTLPrimitiveTopologyClassTriangle;
}
static int hilite_index(NvShader *fs) {
    dispatch_once(&g_hilite_once, ^{
        const char *env = getenv("DM_NVN_HILITE");
        for (const char *q = env; q && *q && g_hilite_n < 6; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) {
            size_t n = MIN(strcspn(q, ","), sizeof g_hilite_keys[0] - 1);
            memcpy(g_hilite_keys[g_hilite_n], q, n); g_hilite_keys[g_hilite_n++][n] = 0;
        }
        const char *full = getenv("DM_NVN_HILITE_FULL"); g_hilite_full = full && !strcmp(full, "1");
        if (!g_hilite_n) return;
        NSError *error = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct O { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]]; float4 c4 [[color(4)]]; };\n"
             "struct V { float4 p [[position]]; };\n"
             "O ho(float4 c) { O o; o.c0=c; o.c1=c; o.c2=c; o.c3=c; o.c4=c; return o; }\n"
             "fragment O hl0(){return ho(float4(1,0,1,1));} fragment O hl1(){return ho(float4(0,1,0,1));}\n"
             "fragment O hl2(){return ho(float4(0,1,1,1));} fragment O hl3(){return ho(float4(1,1,0,1));}\n"
             "fragment O hl4(){return ho(float4(0,0,1,1));} fragment O hl5(){return ho(float4(1,1,1,1));}\n"
             "vertex V hv(uint id [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; V o; o.p=float4(p[id%3],0.5,1); return o; }\n"
            options:nil error:&error];
        for (int i = 0; lib && i < 6; i++) g_hilite_fs[i] = [lib newFunctionWithName:[NSString stringWithFormat:@"hl%d", i]];
        g_hilite_vs = [lib newFunctionWithName:@"hv"];
        if (!lib || !g_hilite_fs[0] || !g_hilite_vs) MLOG("[RED] DM_NVN_HILITE shader compilation failed: %s", error.localizedDescription.UTF8String);
        else MLOG("DM_NVN_HILITE: %d fragment keys%s", g_hilite_n, g_hilite_full ? ", synthetic full-screen vertex output" : "");
    });
    if (!fs) return -1;
    for (int i = 0; i < g_hilite_n; i++) if (!strncmp(fs->key, g_hilite_keys[i], strlen(g_hilite_keys[i]))) return i;
    return -1;
}
static MTLRenderPipelineDescriptor *pso_desc(const pso_key *k, NvShader *vs, NvShader *fs, id<MTLFunction> fsfn, int merged) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.label = [NSString stringWithFormat:@"vs=%s fs=%s topology=%u", vs->key, fs ? fs->key : "-", k->topology];
    d.vertexFunction = vs->fn; d.fragmentFunction = fsfn ? fsfn : fs ? fs->fn : nil;
    if (k->fsfn == TF_MARK) { d.vertexFunction = vs->tf_fn; d.fragmentFunction = nil; d.rasterizationEnabled = NO; }
    // Diagnostic DM_NVN_HILITE=fragment shader key,...: replace matching
    // fragments with solid colors on outputs 0-4. DM_NVN_HILITE_FULL=1 also
    // replaces the vertex function with a full-screen triangle generator.
    int hi = hilite_index(fs);
    if (hi >= 0 && g_hilite_fs[hi]) {
        d.fragmentFunction = g_hilite_fs[hi];
        if (g_hilite_full && g_hilite_vs) d.vertexFunction = g_hilite_vs;
    }
    d.rasterSampleCount = k->samples ? k->samples : 1;
    d.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)k->topology;
    d.alphaToCoverageEnabled = k->a2c;
    for (int i = 0; i < 8; i++) {
        if (!k->cfmt[i]) continue;
        MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
        c.pixelFormat = (MTLPixelFormat)k->cfmt[i];
        int l = merged ? k->lmap[i] : i;
        uint32_t m = l == 0xff ? 0 : (k->chmask >> (4 * l)) & 0xf;
        // Output numbers the fragment shader does not write: zero write mask, no blending. Maxwell writes only the targets in the shader's output map and leaves the others untouched (whatever the blend state and channel mask);
        // on Metal an attachment the shader does not write has undefined contents, and with blending enabled that undefined value is blended in. Super Mario 3D World + Bowser's Fury's volume draws after the G-buffer (50d813761ca3f1f9 and others) write only outputs 0 and 2,
        // while targets 1 (normals) and 3 (pre-lighting) have SRC_ALPHA blending and all channels enabled
        int unwritten = fs && fs->rf.color_known && !fsfn && l != 0xff && !(fs->rf.color_mask >> l & 1);
        if (unwritten) m = 0;
        c.writeMask = (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) | (m & 4 ? MTLColorWriteMaskBlue : 0) | (m & 8 ? MTLColorWriteMaskAlpha : 0);
        if (l != 0xff && k->blend_en >> l & 1 && !unwritten) {
            c.blendingEnabled = YES;
            c.sourceRGBBlendFactor = bfac(k->blend[l].src_rgb); c.destinationRGBBlendFactor = bfac(k->blend[l].dst_rgb);
            c.sourceAlphaBlendFactor = bfac(k->blend[l].src_a); c.destinationAlphaBlendFactor = bfac(k->blend[l].dst_a);
            c.rgbBlendOperation = bop(k->blend[l].eq_rgb); c.alphaBlendOperation = bop(k->blend[l].eq_a);
        }
    }
    d.depthAttachmentPixelFormat = (MTLPixelFormat)k->dfmt; d.stencilAttachmentPixelFormat = (MTLPixelFormat)k->sfmt;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    const nvn_refl *rf = &vs->rf;
    for (int v = 0; v < rf->nv; v++) {
        int loc = rf->v[v].loc, a = rf->v[v].nvn;   // Metal slot loc ← NVN attribute a
        MTLVertexFormat vf = a < 16 ? vfmt(k->attr[a].fmt) : MTLVertexFormatInvalid;
        // When a >= S.nattr, k.attr[a] keeps the 0 from memset, vfmt(0) is Invalid, the same fallback branch is taken and the RED_ONCE condition of fmt being non-0 does not hold.
        if (a < 16 && k->attr[a].fmt && vf == MTLVertexFormatInvalid) RED_ONCE(15, "顶点属性格式 %#x 未映射", k->attr[a].fmt);
        if (vf == MTLVertexFormatInvalid) {
            int ct = rf->v[v].ct;
            vd.attributes[loc].format = ct == NVN_CT_UINT ? MTLVertexFormatUInt4 : ct == NVN_CT_INT ? MTLVertexFormatInt4 : MTLVertexFormatFloat4;
            vd.attributes[loc].offset = 0; vd.attributes[loc].bufferIndex = 30;
            vd.layouts[30].stride = 16; vd.layouts[30].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[30].stepRate = 0;
            continue;
        }
        int st = k->attr[a].stream;
        vf = vfmt_for_ct(vf, rf->v[v].ct, k->attr[a].fmt);
        vd.attributes[loc].format = vf; vd.attributes[loc].offset = k->attr[a].offset; vd.attributes[loc].bufferIndex = VB_SLOT(st);
        vd.layouts[VB_SLOT(st)].stride = k->stream[st].stride ? k->stream[st].stride : 4;
        vd.layouts[VB_SLOT(st)].stepFunction = k->stream[st].stride == 0 ? MTLVertexStepFunctionConstant : k->stream[st].divisor ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
        vd.layouts[VB_SLOT(st)].stepRate = k->stream[st].stride == 0 ? 0 : k->stream[st].divisor ? k->stream[st].divisor : 1;
    }
    d.vertexDescriptor = vd;
    return d;
}
static NSString *nonpoint_digest(NSData *data) {
    if (!data) return nil;
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, hash);
    NSMutableString *s = [NSMutableString stringWithCapacity:64];
    for (unsigned i = 0; i < sizeof hash; i++) [s appendFormat:@"%02x", hash[i]];
    return s;
}
#include "nvn_min_reduction.h"
#include "nvn_guarded_index.h"
// NVN ignores point size for lines/triangles. Older Metal compilers require it
// to be absent from those vertex outputs. Keep the original point/TF functions;
// demote only the non-point variant's raster output to an unused user varying.
// Supplemental sources are exact, hash-checked copies of this chain's inputs,
// exported by dm/ios/tools/export_nonpoint_sources.py (never game assets in git).
static NSDictionary *nonpoint_sources_manifest(void) {
    static NSDictionary *manifest; static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *root = [g_msl_dir stringByAppendingPathComponent:@"nonpoint-v1"];
        NSData *data = [NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:@"manifest.json"]];
        manifest = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    });
    return manifest;
}
static id<MTLFunction> nonpoint_vertex(NvShader *vs, NSError **error) {
    @synchronized (vs) {
        if (!vs->nonpoint_tried) {
            vs->nonpoint_tried = 1;
            // Pipeline prediction can enqueue hundreds of different shaders.
            // Bound source/library/compiler memory while those jobs are pending.
            static dispatch_semaphore_t compile_slots; static dispatch_once_t slots_once;
            dispatch_once(&slots_once, ^{ compile_slots = dispatch_semaphore_create(2); });
            dispatch_semaphore_wait(compile_slots, DISPATCH_TIME_FOREVER);
            NSString *root = [g_msl_dir stringByAppendingPathComponent:@"nonpoint-v1"];
            NSDictionary *manifest = nonpoint_sources_manifest();
            NSString *key = @(vs->key);
            NSDictionary *record = manifest[@"shaders"][key];
            NSData *source = [NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:[key stringByAppendingString:@".metal"]]];
            NSData *library = [NSData dataWithContentsOfFile:[g_msl_dir stringByAppendingPathComponent:[key stringByAppendingString:@".metallib"]] options:NSDataReadingMappedIfSafe error:nil];
            NSError *e = nil;
            BOOL valid = [manifest[@"schema"] isEqual:@1] && [manifest[@"chain"] isEqual:@DMSH_CHAIN]
                && [record[@"source_sha256"] isEqual:nonpoint_digest(source)]
                && [record[@"library_sha256"] isEqual:nonpoint_digest(library)];
            if (valid) source = guarded_index_source(key, source, library, &e);
            NSString *src = valid ? [[NSString alloc] initWithData:source encoding:NSUTF8StringEncoding] : nil;
            if (!src || ![src containsString:@"[[point_size]]"]) {
                if (!e) e = [NSError errorWithDomain:@"NVNNonPoint" code:1 userInfo:@{NSLocalizedDescriptionKey:
                    [NSString stringWithFormat:@"Missing or mismatched non-point source for %@ (chain %s)", key, DMSH_CHAIN]}];
            } else {
                src = [src stringByReplacingOccurrencesOfString:@"[[point_size]]" withString:@"[[user(dm_unused_point_size)]]"];
                MTLCompileOptions *options = [MTLCompileOptions new];
                options.languageVersion = MTLLanguageVersion3_0;
                options.preserveInvariance = YES;
                options.fastMathEnabled = NO; // iOS 16 has no relaxed mathMode; preserve NaN/Inf semantics.
                CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
                vs->nonpoint_lib = [g_dev newLibraryWithSource:src options:options error:&e];
                if (vs->nonpoint_lib) {
                    if (vs->specialized) {
                        vs->nonpoint_fn = [vs->nonpoint_lib newFunctionWithName:vs->fn.name constantValues:[MTLFunctionConstantValues new] error:&e];
                    } else vs->nonpoint_fn = [vs->nonpoint_lib newFunctionWithName:vs->fn.name];
                    if (!vs->nonpoint_fn && !e) e = [NSError errorWithDomain:@"NVNNonPoint" code:2 userInfo:@{NSLocalizedDescriptionKey:@"Non-point entry point missing"}];
                    if (vs->nonpoint_fn && vs->rf.nb && [vs->nonpoint_fn newArgumentEncoderWithBufferIndex:vs->rf.abi].encodedLength != vs->arg_len) {
                        vs->nonpoint_fn = nil;
                        e = [NSError errorWithDomain:@"NVNNonPoint" code:3 userInfo:@{NSLocalizedDescriptionKey:@"Non-point argument layout changed"}];
                    }
                }
                if (vs->nonpoint_fn) MLOG("Non-point vertex variant %s entry=%s compiled in %.1fms; original point/TF entry retained", vs->key, vs->fn.name.UTF8String, (CFAbsoluteTimeGetCurrent() - t0) * 1000);
            }
            if (!vs->nonpoint_fn) {
                vs->nonpoint_error = e;
                MLOG("[RED] Non-point vertex variant %s failed: %s", vs->key, e.localizedDescription.UTF8String);
            }
            dispatch_semaphore_signal(compile_slots);
        }
        if (error) *error = vs->nonpoint_error;
        return vs->nonpoint_fn;
    }
}
static id<MTLRenderPipelineState> pso_build(const pso_key *k, NvShader *vs, NvShader *fs, id<MTLFunction> fsfn, int merged, int specialized, NSError **error) {
    MTLRenderPipelineDescriptor *d = pso_desc(k, vs, fs, fsfn, merged);
    if (!g_bare_native && d.rasterizationEnabled) {
        BOOL attached = d.depthAttachmentPixelFormat || d.stencilAttachmentPixelFormat;
        for (int i = 0; i < 8; i++) attached |= d.colorAttachments[i].pixelFormat != MTLPixelFormatInvalid;
        // Parallel prediction may speculate a format-less descriptor. The
        // actual pass supplies the transient target and a different PSO key.
        // Reject the speculation before legacy Metal's fatal validation.
        if (!attached) {
            if (error) *error = [NSError errorWithDomain:@"NVNMetalCompatibility" code:1 userInfo:@{NSLocalizedDescriptionKey:@"Rasterization without attachments requires Apple4 or Mac2; actual pass uses a transient attachment"}];
            return nil;
        }
    }
    BOOL nonpoint = k->topology != MTLPrimitiveTopologyClassPoint && k->fsfn != TF_MARK && d.vertexFunction == vs->fn;
    // The manifest names exactly the original vertex sources containing a point
    // size output. Make those legal for line/triangle topology before asking
    // Metal to validate the descriptor: a different validation error can mask
    // the point-size conflict, and parallel prediction can finish the variant
    // only after that first failure has already been cached permanently.
    if (nonpoint && nonpoint_sources_manifest()[@"shaders"][@(vs->key)]) {
        NSError *variant_error = nil;
        id<MTLFunction> fn = nonpoint_vertex(vs, &variant_error);
        if (!fn) { if (error) *error = variant_error; return nil; }
        d.vertexFunction = fn;
    } else if (nonpoint) { @synchronized (vs) { if (vs->nonpoint_fn) d.vertexFunction = vs->nonpoint_fn; } }
    id<MTLRenderPipelineState> ps = nvn_cached_render_specialized(g_dev, d, specialized, error);
    NSString *why = error && *error ? (*error).localizedDescription : nil;
    if (!ps && nonpoint && d.vertexFunction == vs->fn && [why containsString:@"writes point size"]) {
        NSError *retry = nil;
        id<MTLFunction> fn = nonpoint_vertex(vs, &retry);
        if (fn) {
            d.vertexFunction = fn; // Never change topology: Metal validates it against each actual draw.
            ps = nvn_cached_render_specialized(g_dev, d, specialized, &retry);
        }
        if (error) *error = ps ? nil : retry ?: *error;
    }
    if (!ps) MLOG("[RED] Pipeline descriptor rejected vs=%s fs=%s actual_formats=%lu/%lu/%lu topology=%lu rasterization=%d vertex=%s fragment=%s nonpoint=%d error=%s",
        vs->key, fs ? fs->key : "-", (unsigned long)d.colorAttachments[0].pixelFormat,
        (unsigned long)d.depthAttachmentPixelFormat, (unsigned long)d.stencilAttachmentPixelFormat,
        (unsigned long)d.inputPrimitiveTopology, d.rasterizationEnabled, d.vertexFunction.name.UTF8String,
        d.fragmentFunction.name.UTF8String, d.vertexFunction == vs->nonpoint_fn, error && *error ? (*error).localizedDescription.UTF8String : "-");
    return ps;
}
// Pipeline key: current state S + pass formats (normal pass; a merged pass additionally carries lmap / specialized function, filled in by pso_get)
static void pso_key_fill(pso_key *k, NvShader *vs, NvShader *fs, const uint32_t cfmt[8], uint32_t dfmt, uint32_t sfmt, uint32_t samples, int lmap) {
    memset(k, 0, sizeof *k);
    k->vs = (__bridge void *)vs; k->fs = (__bridge void *)fs;
    k->topology = g_pso_topology;
    memcpy(k->cfmt, cfmt, sizeof k->cfmt); k->dfmt = dfmt; k->sfmt = sfmt; k->samples = samples;
    k->blend_en = S.color.blend_en; k->chmask = S.chmask.mask; k->a2c = S.ms.a2c;
    for (int i = 0; i < 8; i++) if (k->cfmt[i] || lmap) k->blend[i] = S.blend[i];
    const nvn_refl *rf = &vs->rf;
    for (int v = 0; v < rf->nv; v++) { int a = rf->v[v].nvn; if (a < 16 && a < S.nattr) { k->attr[a] = S.attr[a]; int st = S.attr[a].stream; if (st < 16) k->stream[st] = S.stream[st]; } }
}
// ---- Parallel precompile: pipelines met for the first time used to be compiled synchronously one by one on the submit thread; 9 misses in a row in one frame = 284 ms (heavy scene on a real device).
// Now: on the first pipeline miss in a submission, scan ahead once along the remaining commands (replaying state only, no drawing) and kick all the normal-pass pipelines that will also miss later to the background to compile in parallel;
// when the submit thread reaches that draw it only waits for 'the remaining time not yet compiled'. The compiled pipelines are identical to synchronously compiled ones (same descriptor). Pipeline keys of merged passes / specialized fragments depend on the planning result at replay time, are not predicted, and are compiled synchronously as before.
#define PSO_PEND_MAX 384
static struct { pso_key k; id<MTLRenderPipelineState> ps; int used, done; } g_pp[PSO_PEND_MAX];
static pthread_mutex_t g_pp_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pp_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_pp_submit_mark, g_pp_kicked, g_pp_taken;   // Whether this submission has already been predicted (by submission serial); cumulative kicked / taken counts
static void pso_predict_rest(void);
static int pso_parallel_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *parallel = getenv("DM_NVN_PSO_PARALLEL"), *predict = getenv("DM_NVN_PREDICT");
        on = !((parallel && !strcmp(parallel, "0")) || (predict && !strcmp(predict, "0")));
        if (!on) MLOG("%s=0: predictive pipeline scan disabled; first-use pipelines compile synchronously",
                      parallel && !strcmp(parallel, "0") ? "DM_NVN_PSO_PARALLEL" : "DM_NVN_PREDICT");
    }
    return on;
}
static int pso_pend_find(const pso_key *k) { for (int i = 0; i < PSO_PEND_MAX; i++) if (g_pp[i].used && !memcmp(&g_pp[i].k, k, sizeof *k)) return i; return -1; }
static void pso_pend_kick(const pso_key *k, NvShader *vs, NvShader *fs) {   // Called by the submit thread: kick only if it is neither in the cache nor in the table
    if (hm_get_id(&g_pso, k)) return;
    pthread_mutex_lock(&g_pp_mu);
    int at = pso_pend_find(k), slot = -1;
    if (at < 0) for (int i = 0; i < PSO_PEND_MAX; i++) if (!g_pp[i].used) { slot = i; break; }
    if (at < 0 && slot >= 0) { g_pp[slot].k = *k; g_pp[slot].ps = nil; g_pp[slot].done = 0; g_pp[slot].used = 1; }
    pthread_mutex_unlock(&g_pp_mu);
    if (at >= 0 || slot < 0) return;
    pso_key pk = *k; g_pp_kicked++;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *e = nil; id<MTLRenderPipelineState> ps = pso_build(&pk, vs, fs, nil, 0, vs->specialized || (fs && fs->specialized), &e);
        pthread_mutex_lock(&g_pp_mu); g_pp[slot].ps = ps; g_pp[slot].done = 1; pthread_cond_broadcast(&g_pp_cv); pthread_mutex_unlock(&g_pp_mu);
    });
}
static id<MTLRenderPipelineState> pso_pend_take(const pso_key *k) {   // Called by the submit thread: if it is in the table, wait for it to finish compiling and take it; otherwise return nil
    pthread_mutex_lock(&g_pp_mu);
    int at = pso_pend_find(k); id<MTLRenderPipelineState> ps = nil;
    if (at >= 0) { while (!g_pp[at].done) pthread_cond_wait(&g_pp_cv, &g_pp_mu); ps = g_pp[at].ps; g_pp[at].ps = nil; g_pp[at].used = 0; }
    pthread_mutex_unlock(&g_pp_mu);
    return ps;
}
static void pso_pend_harvest(void) {
    pthread_mutex_lock(&g_pp_mu);
    for (int i = 0; i < PSO_PEND_MAX; i++) {
        if (g_pp[i].used && g_pp[i].done) {
            pso_key k = g_pp[i].k;
            id<MTLRenderPipelineState> ps = g_pp[i].ps;
            g_pp[i].ps = nil; g_pp[i].used = 0; g_pp[i].done = 0;
            if (ps) {
                pthread_mutex_unlock(&g_pp_mu);
                if (!hm_get_id(&g_pso, &k)) {
                    pso_skey sk; char shex[sizeof sk * 2 + 1];
                    pso_skey_from(&k, (__bridge NvShader *)k.vs, (__bridge NvShader *)k.fs, 0, NULL, &sk);
                    pso_skey_hex(&sk, shex);
                    pso_record("R\t%s\n", shex);
                    pso_user_record("R\t%s\n", shex);
                    hm_put_id(&g_pso, &k, ps);
                }
                pthread_mutex_lock(&g_pp_mu);
            }
        }
    }
    pthread_mutex_unlock(&g_pp_mu);
}
static __unsafe_unretained id<MTLRenderPipelineState> pso_get_(NvProg *pr, MTLRenderPassDescriptor *unused);
static id<MTLFunction> ms_fs_override(NvShader *fs);   // DM_NVN_MAGSCAN (defined before draw)
static __unsafe_unretained id<MTLRenderPipelineState> pso_get(NvProg *pr, MTLRenderPassDescriptor *unused) { AP_B; __unsafe_unretained id<MTLRenderPipelineState> r = pso_get_(pr, unused); AP_E(2); return r; }
static __unsafe_unretained id<MTLRenderPipelineState> pso_get_(NvProg *pr, MTLRenderPassDescriptor *unused) {
    (void)unused;
    __unsafe_unretained NvShader *vs = pr->sh[0], *fs = pr->sh[1];
    if (!vs) { RED_ONCE(13, "程序没有顶点着色器（或未映射）"); return nil; }
    pso_key k; pso_key_fill(&k, vs, fs, g_pass.cfmt, g_pass.dfmt, g_pass.sfmt, g_pass.samples, g_pso_lmap != NULL);
    if (g_pso_lmap) { memcpy(k.lmap, g_pso_lmap, 8); k.fsfn = (__bridge void *)g_pso_fsfn; }
    id<MTLFunction> ms_fn = ms_fs_override(fs); if (ms_fn) k.fsfn = (__bridge void *)ms_fn;   // DM_NVN_MAGSCAN phases 2 / 3
    // This variant depends on the actual sampler binding. Keep it distinct
    // from the original pipeline and from readable-tile/diagnostic overrides.
    id<MTLFunction> min_fn = !k.fsfn && !g_pso_lmap ? min_reduction_function(fs) : nil;
    if (min_fn) k.fsfn = (__bridge void *)min_fn;
    __unsafe_unretained id<MTLRenderPipelineState> cached = hm_get_id(&g_pso, &k);
    if (cached) return cached;
    pso_pend_harvest();
    cached = hm_get_id(&g_pso, &k);
    if (cached) return cached;
    if (hm_get_id(&g_pso_fail, &k)) {
        if (min_fn) { fs->min_reduction_pso_failed = 1; return pso_get_(pr, unused); }
        return nil;
    }
    id<MTLRenderPipelineState> ps;
    NSError *e = nil; uint64_t bt = bp_now();
    ps = g_pso_lmap || ms_fn || min_fn ? nil : pso_pend_take(&k);
    if (ps) g_pp_taken++;
    else {
        if (pso_parallel_on() && g_pp_submit_mark != g_sub_serial) { g_pp_submit_mark = g_sub_serial; pso_predict_rest(); }   // First miss in this submission: first kick the later ones out to compile in parallel, then synchronously compile the one at hand
        ps = pso_build(&k, vs, fs, ms_fn ? ms_fn : min_fn ? min_fn : g_pso_fsfn, g_pso_lmap != NULL,
                       k.fsfn != NULL || vs->specialized || (fs && fs->specialized), &e);
    }
    g_bp_pso_n++; g_bp_pso_ns += bp_now() - bt;
    if (!ps) { MLOG("[RED] Render pipeline failed vs=%s fs=%s pass=%llu formats=%u/%u/%u depth=%d/%d bound-depth=%llu dt=%d dw=%d topology=%u: %s", vs->key, fs ? fs->key : "-", (unsigned long long)g_pass.serial, k.cfmt[0], k.dfmt, k.sfmt, g_pass.hd, g_pass.hs, (unsigned long long)(R.rt.depth ? R.rt.depth->serial : 0), S.ds.dt, S.ds.dw, k.topology, e.localizedDescription.UTF8String); hm_put_id(&g_pso_fail, &k, NSNull.null);
        if (min_fn) { fs->min_reduction_pso_failed = 1;
            MLOG("Min-reduction %s pipeline rejected; disabling its companion and retrying the ordinary pipeline", fs->key);
            return pso_get_(pr, unused); }
        return nil; }
    if (!ms_fn && !min_fn) { pso_skey sk; char shex[sizeof sk * 2 + 1]; pso_skey_from(&k, vs, fs, g_pso_lmap != NULL, g_pso_lmap ? &g_pso_fz : NULL, &sk); pso_skey_hex(&sk, shex);
        pso_record("R\t%s\n", shex);
        pso_user_record("R\t%s\n", shex); }
    hm_put_id(&g_pso, &k, ps);
    return ps;
}
static NSString *warm_key(const char *s, size_t n) {
    size_t z = strnlen(s, n);
    return [[NSString alloc] initWithBytes:s length:z encoding:NSUTF8StringEncoding];
}
static NvShader *warm_shader(NSMutableDictionary<NSString *, NvShader *> *dict, NSString *key, int stage) {
    NvShader *sh = dict[key];
    if (!sh) { sh = shader_load(key, stage); if (sh) dict[key] = sh; }
    return sh;
}
static void warm_enqueue(WarmItem *item) {
    os_unfair_lock_lock(&g_warm_lock);
    [g_warm_items addObject:item];
    atomic_store_explicit(&g_warm_ready, 1, memory_order_release);
    os_unfair_lock_unlock(&g_warm_lock);
}
// 0 = prepared, 1 = failure, 2 = unsupported recorded specialization.
static int warm_row(NSString *line, NSMutableDictionary<NSString *, NvShader *> *dict) {
    NSArray<NSString *> *c = [line componentsSeparatedByString:@"\t"];
    if ([c[0] isEqualToString:@"S"] && c.count >= 3) {
        NSString *key = c[1]; NvShader *sh = warm_shader(dict, key, c[2].intValue);
        if (!sh) return 1;
        WarmItem *item = [WarmItem new]; item->kind = 0; item->key = key; item->shader = sh;
        warm_enqueue(item); g_warm_shaders++; return 0;
    }
    if ([c[0] isEqualToString:@"R"] && c.count >= 2) {
        pso_skey sk; if (pso_skey_parse(c[1].UTF8String, &sk)) return 1;
        NSString *vkey = warm_key(sk.vs, sizeof sk.vs), *fkey = warm_key(sk.fs, sizeof sk.fs);
        NvShader *vs = vkey.length ? warm_shader(dict, vkey, sk.vs_stage) : nil;
        NvShader *fs = fkey.length ? warm_shader(dict, fkey, sk.fs_stage) : nil;
        if (!vs || (fkey.length && !fs)) return 1;
        id<MTLFunction> fsfn = nil;
        if (sk.fz) {
            if (sk.fz_desc.kind == 1 && fs) { int fx[32]; for (int i = 0; i < 32; i++) fx[i] = sk.fz_desc.fx[i]; fsfn = bf_fn(fs, fx, sk.fz_desc.fbmask, sk.k.lmap); }
            if (!fsfn) { g_warm_skip_fz++; return 2; }
        }
        pso_key k = sk.k; k.vs = (__bridge void *)vs; k.fs = (__bridge void *)fs; k.fsfn = (__bridge void *)fsfn;
        NSError *e = nil; id ps = pso_build(&k, vs, fs, fsfn, sk.merged, sk.fz || vs->specialized || (fs && fs->specialized), &e);
        if (!ps) { MLOG("【红】Startup render pipeline failed: %s", e.localizedDescription.UTF8String); return 1; }
        WarmItem *item = [WarmItem new]; item->kind = 1; item->vs_key = vkey; item->fs_key = fkey; item->vs = vs; item->fs = fs; item->fsfn = fsfn; item->pso = k; item->object = ps;
        warm_enqueue(item); g_warm_rps++; return 0;
    }
    if ([c[0] isEqualToString:@"C"] && c.count >= 2) {
        NSString *key = c[1]; if (!key.length) return 1;
        NvShader *cs = warm_shader(dict, key, 5); if (!cs) return 1;
        NSError *e = nil; id ps = nvn_cached_compute(g_dev, cs->fn, &e);
        if (!ps) { MLOG("【红】Startup compute pipeline failed: %s", e.localizedDescription.UTF8String); return 1; }
        WarmItem *item = [WarmItem new]; item->kind = 2; item->key = key; item->shader = cs; item->object = ps;
        warm_enqueue(item); g_warm_cps++; return 0;
    }
    return 1;
}
static void warm_worker(NSArray<NSString *> *paths) {
    @autoreleasepool {
        NSMutableDictionary<NSString *, NvShader *> *dict = [NSMutableDictionary dictionary];
        NSMutableOrderedSet<NSString *> *rows = [NSMutableOrderedSet orderedSet];
        for (NSString *path in paths) {
            NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
            if (!txt) { atomic_fetch_add(&g_prepare_failed, 1); MLOG("【红】Warmup manifest unavailable: %s", path.UTF8String); continue; }
            for (NSString *raw in [txt componentsSeparatedByString:@"\n"]) {
                NSString *line = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
                if (line.length && ![line hasPrefix:@"#"]) [rows addObject:line];
            }
        }
        atomic_store(&g_prepare_total, (uint32_t)rows.count);
        if (!rows.count) atomic_fetch_add(&g_prepare_failed, 1);
        if (atomic_load(&g_prepare_frontend)) atomic_store(&g_prepare_state, NVN_PREP_WORKING);
        for (NSString *line in rows) {
            prepare_foreground_wait();
            @autoreleasepool {
                int result = warm_row(line, dict);
                if (result == 1) atomic_fetch_add(&g_prepare_failed, 1);
                if (result == 2) atomic_fetch_add(&g_prepare_skipped, 1);
                uint32_t done = atomic_fetch_add(&g_prepare_done, 1) + 1;
                // Before guest startup there is only one owner of these tables.
                // Drain transient warm entries regularly without drawing any frame.
                if (atomic_load(&g_prepare_frontend) && done % 64 == 0) warm_claim();
            }
        }
        MLOG("Pipeline preparation: known=%u done=%u failed=%u skipped=%u shaders=%llu render=%llu compute=%llu elapsed=%.1fms",
            atomic_load(&g_prepare_total), atomic_load(&g_prepare_done), atomic_load(&g_prepare_failed), atomic_load(&g_prepare_skipped),
            g_warm_shaders, g_warm_rps, g_warm_cps, (bp_now() - g_warm_t0) / 1e6);
    }
    atomic_store(&g_warm_running, 0);
}
// Wait for the background warm-up to finish (at most timeout_ms): returns how many milliseconds were actually waited, 0 if it is not running. On first install the system shader cache is fully cold; if warm-up is slower than the player entering a run,
// pipelines not yet warmed are compiled on the spot on the submit thread = in-run stutter; the loading screen on entering a run blocks the player anyway, so let it load a little longer (called where hle_nvn.c presents).
int mtl_warm_wait(int timeout_ms) {
    if (!atomic_load(&g_warm_running)) return 0;
    uint64_t t0 = bp_now();
    while (atomic_load(&g_warm_running) && (bp_now() - t0) / 1000000ull < (uint64_t)timeout_ms) usleep(2000);
    return (int)((bp_now() - t0) / 1000000ull) + 1;
}
static void warm_start(void) {
    if (g_warm_started++) return;
    const char *e = getenv("DM_PSO_MANIFEST");
    if (e && !strcmp(e, "off")) { atomic_fetch_add(&g_prepare_skipped, 1); return; }
    NSArray<NSString *> *paths;
    if (e && *e) {
        NSMutableArray *all = [NSMutableArray arrayWithObject:@(e)];
        NSString *user = pso_user_path();
        if (user && ![user isEqualToString:@(e)] && [NSFileManager.defaultManager fileExistsAtPath:user]) [all addObject:user];
        paths = all;
    } else {
        NSString *seed = [g_shmap_path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pso_manifest.tsv"];
        NSMutableArray<NSString *> *all = [NSMutableArray arrayWithObject:seed];
        NSString *user = pso_user_path();
        if (user && [NSFileManager.defaultManager fileExistsAtPath:user]) [all addObject:user];
        else MLOG("管线预热：本机用户清单%s%s", user ? "还没有：" : "没有路径（DM_PSO_USER / DM_SAVE 都没设）", user ? user.UTF8String : "");
        if (![NSFileManager.defaultManager fileExistsAtPath:seed]) MLOG("【红】管线预热：种子清单不存在：%s", seed.UTF8String);
        paths = all;
    }
    if (!g_warm_items) g_warm_items = [NSMutableArray array];
    g_warm_t0 = bp_now();
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0);   // Not optional background work: it decides whether the pipelines are ready before the player enters a run
    dispatch_queue_t q = dispatch_queue_create("dm.nvn.pso-warm", attr);
    atomic_store(&g_warm_running, 1);
    if (atomic_load(&g_prepare_frontend)) warm_worker(paths);
    else dispatch_async(q, ^{ warm_worker(paths); });
}
void mtl_prepare_start(void) {
    int expected = NVN_PREP_IDLE;
    if (!atomic_compare_exchange_strong(&g_prepare_state, &expected, NVN_PREP_LOADING)) return;
    atomic_store(&g_prepare_frontend, 1);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            prepare_foreground_wait();
            const char *map = getenv("DM_NVN_SHMAP"), *msl = getenv("DM_MSL_DIR");
            NSString *libraries = msl ? [@(msl) stringByAppendingPathComponent:@"b-" DMSH_CHAIN] : nil;
            if (!map || !libraries || ![NSFileManager.defaultManager fileExistsAtPath:@(map)] ||
                ![NSFileManager.defaultManager fileExistsAtPath:libraries]) {
                atomic_fetch_add(&g_prepare_failed, 1); atomic_store(&g_prepare_state, NVN_PREP_FAILED);
                MLOG("【红】Startup preparation missing shader map/libraries"); return;
            }
            mtl_init(); shmap_load(); // warm_start runs synchronously on this worker.
            warm_claim();
            prepare_foreground_wait();
            atomic_store(&g_prepare_state, NVN_PREP_SAVING);
            nvn_cache_flush();
            nvn_cache_status cs; nvn_cache_snapshot(&cs);
            MLOG("Startup gate ready: hits=%llu misses=%llu bypassed=%llu persisted=%llu failed_write=%d", cs.hits, cs.misses, cs.bypassed, cs.saved, cs.write_failed);
            atomic_store(&g_prepare_state, atomic_load(&g_prepare_failed) || atomic_load(&g_prepare_skipped) || cs.write_failed ? NVN_PREP_PARTIAL : NVN_PREP_READY);
        }
    });
}
static int g_rt_ovr;   // Stencil override for runtime extrapolation: 0 no override; 1 pass only on the unit / effect / extrapolation-hole bits (0xD0), no stencil write; 3 depth test as usual, write the effect bit 0x40 where it passes
static id<MTLDepthStencilState> dss_get(int has_depth, int has_stencil) {
    static int diag = -1;
    if (diag < 0) { const char *e = getenv("DM_NVN_DEPTH_ALWAYS"); diag = e && !strcmp(e, "1"); if (diag) MLOG("DM_NVN_DEPTH_ALWAYS=1: force depth compare Always and disable stencil tests"); }
    struct { ds_st ds; uint32_t vm[2], wm[2]; int hd, hs, ovr, diag; } k;
    memset(&k, 0, sizeof k); k.ds = S.ds; memcpy(k.vm, S.st_vmask, 8); memcpy(k.wm, S.st_wmask, 8); k.hd = has_depth; k.hs = has_stencil; k.ovr = has_stencil ? g_rt_ovr : 0; k.diag = diag;
    if (!g_dss.klen) g_dss.klen = sizeof k;
    id<MTLDepthStencilState> s = hm_get_id(&g_dss, &k);
    if (s) return s;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    if (has_depth && diag) { d.depthCompareFunction = MTLCompareFunctionAlways; d.depthWriteEnabled = S.ds.dw; }
    else if (has_depth && S.ds.dt) { d.depthCompareFunction = (MTLCompareFunction)(S.ds.df - 1); d.depthWriteEnabled = S.ds.dw; }
    if (has_stencil && S.ds.st && !diag) {
        MTLStencilDescriptor *f = [MTLStencilDescriptor new], *b = [MTLStencilDescriptor new];
        f.stencilCompareFunction = (MTLCompareFunction)(S.ds.sf_f - 1); f.stencilFailureOperation = (MTLStencilOperation)(S.ds.fail_f - 1);
        f.depthFailureOperation = (MTLStencilOperation)(S.ds.zfail_f - 1); f.depthStencilPassOperation = (MTLStencilOperation)(S.ds.zpass_f - 1);
        f.readMask = S.st_vmask[0]; f.writeMask = S.st_wmask[0];
        b.stencilCompareFunction = (MTLCompareFunction)(S.ds.sf_b - 1); b.stencilFailureOperation = (MTLStencilOperation)(S.ds.fail_b - 1);
        b.depthFailureOperation = (MTLStencilOperation)(S.ds.zfail_b - 1); b.depthStencilPassOperation = (MTLStencilOperation)(S.ds.zpass_b - 1);
        b.readMask = S.st_vmask[1]; b.writeMask = S.st_wmask[1];
        d.frontFaceStencil = f; d.backFaceStencil = b;
    }
    if (has_stencil && k.ovr) {
        MTLStencilDescriptor *f = [MTLStencilDescriptor new];
        if (k.ovr == 1) { f.stencilCompareFunction = MTLCompareFunctionNotEqual; f.readMask = 0xD0; f.writeMask = 0; }   // Reference value 0: passes only if (s & 0xD0) != 0 (unit 0x10, effect 0x40, extrapolation hole / screen edge 0x80)
        else { f.stencilCompareFunction = MTLCompareFunctionAlways; f.depthStencilPassOperation = MTLStencilOperationReplace; f.readMask = 0; f.writeMask = 0x40; }   // Reference value 0x40
        d.frontFaceStencil = f; d.backFaceStencil = f;
    }
    s = [g_dev newDepthStencilStateWithDescriptor:d];
    hm_put_id(&g_dss, &k, s);
    return s;
}
// Argument buffer: filled item by item per the B reflection (uniform slot r, separate texture / sampler slot r, storage buffer slot r, driver segments c0 / c2, c1, support_buffer)
// Unbound resources read 0: give all-zero dummy resources (NVN descriptors 0..255 are reserved for the driver; the game binding handle 0 means 'this slot is empty')
static uint64_t g_dummy_tex_rid[8][4], g_dummy_img_rid[8][4], g_dummy_tbuf_rid[4];
static id<MTLTexture> dummy_tex(int dtt, int dpf) {
    static __strong id<MTLTexture> cache[8][4];
    MTLTextureType tt = (MTLTextureType)dtt;
    MTLPixelFormat pf = dpf == NVN_DPF_DEPTH ? MTLPixelFormatDepth32Float : dpf == NVN_DPF_UINT ? MTLPixelFormatRGBA32Uint : dpf == NVN_DPF_SINT ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA8Unorm;
    id<MTLTexture> t = cache[dtt & 7][dpf & 3];
    if (t) return t;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = tt; d.pixelFormat = pf; d.width = d.height = d.depth = 1; d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    t = [g_dev newTextureWithDescriptor:d];   // Shared storage is all zeros when newly created
    if (pf != MTLPixelFormatDepth32Float) { uint8_t z[64] = {0}; NSUInteger faces = tt == MTLTextureTypeCube || tt == MTLTextureTypeCubeArray ? 6 : 1;
        for (NSUInteger f = 0; f < faces; f++) [t replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:f withBytes:z bytesPerRow:16 bytesPerImage:16]; }
    cache[dtt & 7][dpf & 3] = t; g_dummy_tex_rid[dtt & 7][dpf & 3] = legacy_texture_rid(t); res_add(t);
    return t;
}
// Write sink for unbound image slots: kept separate from the dummy textures that read 0 (writes must not pollute them); one is shared per same type and same pixel class
static id<MTLTexture> dummy_img(int dtt, int dpf) {
    static __strong id<MTLTexture> cache[8][4];
    id<MTLTexture> t = cache[dtt & 7][dpf & 3]; if (t) return t;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = (MTLTextureType)dtt; d.pixelFormat = dpf == NVN_DPF_UINT ? MTLPixelFormatRGBA32Uint : dpf == NVN_DPF_SINT ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA16Float;
    d.width = d.height = d.depth = 1; d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    t = [g_dev newTextureWithDescriptor:d]; cache[dtt & 7][dpf & 3] = t; g_dummy_img_rid[dtt & 7][dpf & 3] = legacy_texture_rid(t); res_add(t); return t;
}
static id<MTLBuffer> dummy_buf(void);
static id<MTLTexture> dummy_tbuf(int dpf) {   // Unbound typed buffer: all zeros (pixel class matches the shader's texture_buffer<T>)
    static __strong id<MTLTexture> cache[4];
    id<MTLTexture> t = cache[dpf & 3]; if (t) return t;
    MTLPixelFormat pf = dpf == NVN_DPF_UINT ? MTLPixelFormatRGBA32Uint : dpf == NVN_DPF_SINT ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA32Float;
    id<MTLBuffer> b = dummy_buf();
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:pf width:b.length / 16 resourceOptions:b.resourceOptions usage:MTLTextureUsageShaderRead];
    t = [b newTextureWithDescriptor:d offset:0 bytesPerRow:b.length];
    cache[dpf & 3] = t; g_dummy_tbuf_rid[dpf & 3] = legacy_texture_rid(t); return t;
}
static id<MTLSamplerState> dummy_samp(void) {
    static id<MTLSamplerState> s; if (!s) { MTLSamplerDescriptor *d = [MTLSamplerDescriptor new]; d.supportArgumentBuffers = YES; s = [g_dev newSamplerStateWithDescriptor:d]; }
    return s;
}
static uint64_t dummy_samp_rid(void) { static uint64_t rid; if (!rid) rid = legacy_sampler_rid(dummy_samp()); return rid; }
static id<MTLBuffer> dummy_buf(void) { static id<MTLBuffer> b; if (!b) { b = [g_dev newBufferWithLength:64 << 10 options:MTLResourceStorageModeShared]; res_add(b); } return b; }
static uint64_t dummy_buf_gpu(void) { static uint64_t gpu; if (!gpu) gpu = legacy_buffer_gpu(dummy_buf()); return gpu; }
// A shader can still calculate image-atomic addresses from the dimensions of a
// bound block-linear texture even though its guest layout cannot be exposed as
// a linear Metal buffer. Keep those writes inside a sink large enough for the
// logical image rather than the generic 64 KB unbound-resource buffer.
static id<MTLBuffer> imga_dummy(size_t need) {
    static id<MTLBuffer> b;
    if (need < (64u << 10)) need = 64u << 10;
    if (need > SIZE_MAX - 0xffffu) MDIE("Image-atomic sink size overflows: %zu bytes", need);
    need = (need + 0xffffu) & ~(size_t)0xffffu;
    if (!b || b.length < need) {
        id<MTLBuffer> next = [g_dev newBufferWithLength:need options:MTLResourceStorageModeShared];
        // Binding the generic 64 KB dummy here would leave valid image
        // coordinates outside the allocation. Fail before GPU encoding.
        if (!next || next.length < need) MDIE("Cannot allocate %zu-byte image-atomic sink", need);
        id<MTLBuffer> old = b;
        b = next;
        res_add(next);
        if (old) defer_obj_release(old);
    }
    return b;
}

// Z-A's 2bd5037003980716_b first clears a 480x270x3 R32Uint image, then
// performs atomicMax through SPIRV-Cross's linear-buffer companion.  The next
// pass samples that same image. A block-linear NVN image resolves
// to a private Metal texture, so the pointer and the texture otherwise refer
// to different storage.  Keep one zeroed shadow per submission allocator and
// copy every completed array plane before the consumer pass. The bounded
// shader variant uses the same packed row/slice layout as this companion.
static __strong id<MTLBuffer> g_za_atomic_ring[ALLOC_MAX];
static size_t g_za_atomic_ring_size[ALLOC_MAX];
static uint64_t g_za_atomic_ring_submit[ALLOC_MAX];
static int g_za_atomic_pending;
static __strong id<MTLBuffer> g_za_atomic_pending_buf;
static __strong id<MTLTexture> g_za_atomic_pending_tex;
static NSUInteger g_za_atomic_pending_w, g_za_atomic_pending_h;

static id<MTLBuffer> za_atomic_shadow(size_t need) {
    int k = g_alloc_cur;
    if (k < 0 || k >= ALLOC_MAX) return nil;
    if (need < (64u << 10)) need = 64u << 10;
    if (need > SIZE_MAX - 0xffffu) return nil;
    need = (need + 0xffffu) & ~(size_t)0xffffu;
    if (!g_za_atomic_ring[k] || g_za_atomic_ring_size[k] < need) {
        id<MTLBuffer> old = g_za_atomic_ring[k];
        id<MTLBuffer> b = [g_dev newBufferWithLength:need options:MTLResourceStorageModeShared];
        if (!b) return nil;
        g_za_atomic_ring[k] = b; g_za_atomic_ring_size[k] = need; g_za_atomic_ring_submit[k] = 0;
        res_add(b); if (old) defer_obj_release(old);
    }
    if (g_za_atomic_ring_submit[k] != g_sub_serial) {
        memset(g_za_atomic_ring[k].contents, 0, g_za_atomic_ring[k].length);
        g_za_atomic_ring_submit[k] = g_sub_serial;
    }
    return g_za_atomic_ring[k];
}

static void za_atomic_schedule(id<MTLBuffer> b, id<MTLTexture> t) {
    if (!b || !t || t.textureType != MTLTextureType2DArray || t.pixelFormat != MTLPixelFormatR32Uint) {
        RED_ONCE(94, "Z-A image-atomic shadow has an unsupported destination (buffer %p, texture type %lu format %lu)",
                 (__bridge void *)b, (unsigned long)t.textureType, (unsigned long)t.pixelFormat);
        return;
    }
    g_za_atomic_pending = 1; g_za_atomic_pending_buf = b; g_za_atomic_pending_tex = t;
    g_za_atomic_pending_w = t.width; g_za_atomic_pending_h = t.height;
}

static void za_atomic_sync_end(void) {
    if (!g_za_atomic_pending) return;
    g_za_atomic_pending = 0;
    id<MTLBuffer> b = g_za_atomic_pending_buf; id<MTLTexture> t = g_za_atomic_pending_tex;
    NSUInteger w = g_za_atomic_pending_w, h = g_za_atomic_pending_h;
    g_za_atomic_pending_buf = nil; g_za_atomic_pending_tex = nil;
    size_t row = (size_t)w * 4, image = row * (size_t)h, layers = t.arrayLength;
    if (!w || !h || !layers || image / row != h || layers > b.length / image) {
        RED_ONCE(95, "Z-A image-atomic shadow copy is out of range (%lux%lu, %zu-byte buffer)",
                 (unsigned long)w, (unsigned long)h, (size_t)b.length);
        return;
    }
    id<MTLBlitCommandEncoder> bl = enc_b_begin(g_cb);
    for (NSUInteger layer = 0; layer < layers; layer++)
        enc_b_copy_buffer_texture(bl, b, layer * image, row, image, MTLSizeMake(w, h, 1), t, layer, 0, MTLOriginMake(0, 0, 0));
    [bl fillBuffer:b range:NSMakeRange(0, image * layers) value:0];
    enc_b_end(bl);
    static int once;
    if (!once++) MLOG("Z-A image atomics: synchronized %lux%lux%lu R32Uint shadow before the consumer pass",
                      (unsigned long)w, (unsigned long)h, (unsigned long)layers);
}
#include "nvn_zero_image_runtime.h"

// Pokémon Legends: Z-A black-frame diagnostic.  Its fragment shader
// 2bd5037003980716_b has no colour output: it rasterizes into an R32Uint image
// through an atomic buffer pointer.  On Metal the block-linear image and the
// linear atomic pointer do not share storage, so record the complete producer →
// consumer chain before changing its semantics.  DM_NVN_ZA_DIAG=1 captures at
// most twelve frames.  Every other value leaves the production path with no
// diagnostic references, allocations, blits or readbacks.
static int g_za_diag_armed, g_za_diag_frames, g_za_diag_use_n;
static uint64_t g_za_diag_last_frame = UINT64_MAX, g_za_diag_ser, g_za_diag_src_ser;
static int g_za_diag_tid, g_za_diag_src_tid;
static size_t g_za_diag_atomic_bytes;
static __strong id<MTLBuffer> g_za_diag_atomic;
static __strong id<MTLTexture> g_za_diag_out, g_za_diag_src;

static int za_diag_enabled(void) {
    static dispatch_once_t once; static int enabled;
    dispatch_once(&once, ^{
        const char *value = getenv("DM_NVN_ZA_DIAG");
        enabled = value && !strcmp(value, "1");
    });
    return enabled;
}

static void za_diag_words(const char *what, uint64_t frame, uint64_t pass, int tid, uint64_t ser,
                          const void *data, size_t bytes) {
    const uint32_t *p = data; size_t n = bytes / 4, nz = 0; uint32_t lo = UINT32_MAX, hi = 0;
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = p[i]; hash ^= v; hash *= 1099511628211ull;
        if (v) { nz++; if (v < lo) lo = v; if (v > hi) hi = v; }
    }
    MLOG("[ZA-DIAG] %s frame=%llu pass=%llu tid=%d ser=%llu bytes=%zu nonzero_words=%zu min=%#x max=%#x hash=%016llx",
         what, (unsigned long long)frame, (unsigned long long)pass, tid, (unsigned long long)ser,
         bytes, nz, nz ? lo : 0, hi, (unsigned long long)hash);
}

static size_t za_diag_texture_bytes(id<MTLTexture> t, NSUInteger *slices, size_t *row, size_t *image) {
    if (!t) return 0;
    size_t bpp = mtl_pf_bytes(t.pixelFormat); if (!bpp) return 0;
    NSUInteger ns = 1;
    if (t.textureType == MTLTextureType2DArray) ns = MAX(t.arrayLength, 1u);
    else if (t.textureType == MTLTextureTypeCube) ns = 6;
    else if (t.textureType == MTLTextureTypeCubeArray) ns = MAX(t.arrayLength, 1u) * 6;
    else if (t.textureType == MTLTextureType3D) ns = MAX(t.depth, 1u);
    size_t r = (size_t)t.width * bpp, im = r * (size_t)t.height;
    if (!r || im / r != t.height || ns > SIZE_MAX / im) return 0;
    *slices = ns; *row = r; *image = im; return im * ns;
}

static void za_diag_arm(NvShader *sh, int stage, int reg, int tid, const tex_t *tt,
                        id<MTLBuffer> atomic, size_t atomic_bytes) {
    if (!za_diag_enabled()) return;
    if (!tt || strcmp(sh->key, "2bd5037003980716_b")) return;
    uint64_t frame = atomic_load(&g_cur_frame);
    g_za_diag_ser = tt->serial; g_za_diag_tid = tid;
    if (g_za_diag_armed || frame == g_za_diag_last_frame || g_za_diag_frames >= 12) return;
    g_za_diag_last_frame = frame; g_za_diag_frames++;
    int src_tid = stage < 6 ? (int)(S.tex[stage][0] & 0xfffff) : 0;
    const tex_t *src_tt = src_tid > 0 && src_tid < g_ntreg && g_treg[src_tid].ok ? &g_treg[src_tid].t : NULL;
    void *outv = NULL, *srcv = NULL;
    (void)tex_fast_rid(tid, &outv); if (src_tid) (void)tex_fast_rid(src_tid, &srcv);
    g_za_diag_armed = 1; g_za_diag_atomic = atomic; g_za_diag_atomic_bytes = atomic_bytes;
    g_za_diag_out = (__bridge id<MTLTexture>)outv; g_za_diag_src = (__bridge id<MTLTexture>)srcv;
    g_za_diag_src_tid = src_tid; g_za_diag_src_ser = src_tt ? src_tt->serial : 0;
    MLOG("[ZA-DIAG] arm frame=%llu pass=%llu shader=%s stage=%d image_reg=%d out_tid=%d ser=%llu nvn=%#x target=%u flags=%#x size=%ux%ux%u levels=%u layout=%llu atomic_bytes=%zu metal=%lu %lux%lux%lu array=%lu type=%lu usage=%#lx src_tid=%d src_ser=%llu src_metal=%lu %lux%lux%lu type=%lu",
         (unsigned long long)frame, (unsigned long long)g_stat_pass, sh->key, stage, reg, tid,
         (unsigned long long)tt->serial, tt->d.format, tt->d.target, tt->d.flags, tt->d.width, tt->d.height,
         tt->d.depth, tt->d.levels, (unsigned long long)tt->L.size, atomic_bytes,
         (unsigned long)g_za_diag_out.pixelFormat, (unsigned long)g_za_diag_out.width,
         (unsigned long)g_za_diag_out.height, (unsigned long)g_za_diag_out.depth,
         (unsigned long)g_za_diag_out.arrayLength, (unsigned long)g_za_diag_out.textureType,
         (unsigned long)g_za_diag_out.usage, src_tid, (unsigned long long)g_za_diag_src_ser,
         (unsigned long)g_za_diag_src.pixelFormat, (unsigned long)g_za_diag_src.width,
         (unsigned long)g_za_diag_src.height, (unsigned long)g_za_diag_src.depth,
         (unsigned long)g_za_diag_src.textureType);
}

static void za_diag_use(const char *kind, NvShader *sh, int stage, int reg, int tid, uint64_t ser) {
    if (!za_diag_enabled()) return;
    if (!g_za_diag_ser || (tid != g_za_diag_tid && ser != g_za_diag_ser)) return;
    if (g_za_diag_use_n++ < 160)
        MLOG("[ZA-DIAG] consume frame=%llu pass=%llu kind=%s shader=%s stage=%d reg=%d tid=%d ser=%llu",
             (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)g_stat_pass, kind,
             sh && sh->key[0] ? sh->key : "-", stage, reg, tid, (unsigned long long)ser);
}

static void za_diag_op(const char *kind, const tex_t *t) {
    if (!za_diag_enabled()) return;
    if (!t || !g_za_diag_ser || t->serial != g_za_diag_ser) return;
    if (g_za_diag_use_n++ < 160)
        MLOG("[ZA-DIAG] texture-op frame=%llu pass=%llu kind=%s ser=%llu nvn=%#x size=%ux%ux%u",
             (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)g_stat_pass, kind,
             (unsigned long long)t->serial, t->d.format, t->d.width, t->d.height, t->d.depth);
}

static void za_diag_pass_end(void) {
    if (!za_diag_enabled()) return;
    if (!g_za_diag_armed) return;
    g_za_diag_armed = 0;
    uint64_t frame = g_za_diag_last_frame, pass = g_stat_pass, ser = g_za_diag_ser, src_ser = g_za_diag_src_ser;
    int tid = g_za_diag_tid, src_tid = g_za_diag_src_tid;
    id<MTLBuffer> atomic = g_za_diag_atomic; size_t atomic_bytes = MIN(g_za_diag_atomic_bytes, atomic.length);
    id<MTLTexture> out = g_za_diag_out, src = g_za_diag_src;
    g_za_diag_atomic = nil; g_za_diag_out = nil; g_za_diag_src = nil;

    NSUInteger out_slices = 0, src_slices = 0; size_t out_row = 0, out_img = 0, src_row = 0, src_img = 0;
    size_t out_bytes = za_diag_texture_bytes(out, &out_slices, &out_row, &out_img);
    size_t src_bytes = za_diag_texture_bytes(src, &src_slices, &src_row, &src_img);
    if (out_bytes > (64u << 20)) out_bytes = 0;
    if (src_bytes > (64u << 20)) src_bytes = 0;
    id<MTLBuffer> out_rb = out_bytes ? [g_dev newBufferWithLength:out_bytes options:MTLResourceStorageModeShared] : nil;
    id<MTLBuffer> src_rb = src_bytes ? [g_dev newBufferWithLength:src_bytes options:MTLResourceStorageModeShared] : nil;
    if (out_rb) res_add(out_rb); if (src_rb) res_add(src_rb);
    if (out_rb || src_rb) {
        id<MTLBlitCommandEncoder> b = enc_b_begin(g_cb);
        if (out_rb) for (NSUInteger s = 0; s < out_slices; s++)
            enc_b_copy_texture_buffer(b, out, out.textureType == MTLTextureType3D ? 0 : s, 0,
                                      MTLOriginMake(0, 0, out.textureType == MTLTextureType3D ? s : 0),
                                      MTLSizeMake(out.width, out.height, 1), out_rb, s * out_img, out_row, out_img);
        if (src_rb) for (NSUInteger s = 0; s < src_slices; s++)
            enc_b_copy_texture_buffer(b, src, src.textureType == MTLTextureType3D ? 0 : s, 0,
                                      MTLOriginMake(0, 0, src.textureType == MTLTextureType3D ? s : 0),
                                      MTLSizeMake(src.width, src.height, 1), src_rb, s * src_img, src_row, src_img);
        enc_b_end(b);
    }
    [g_commit_blocks addObject:^{
        za_diag_words("atomic-buffer-after-producer", frame, pass, tid, ser, atomic.contents, atomic_bytes);
        if (out_rb) za_diag_words("output-texture-after-producer", frame, pass, tid, ser, out_rb.contents, out_bytes);
        else MLOG("[ZA-DIAG] output texture readback unavailable frame=%llu pass=%llu tid=%d ser=%llu", (unsigned long long)frame, (unsigned long long)pass, tid, (unsigned long long)ser);
        if (src_rb) za_diag_words("source-texture-at-producer", frame, pass, src_tid, src_ser, src_rb.contents, src_bytes);
        else MLOG("[ZA-DIAG] source texture readback unavailable frame=%llu pass=%llu tid=%d ser=%llu", (unsigned long long)frame, (unsigned long long)pass, src_tid, (unsigned long long)src_ser);
    }];
}
// useResource is idempotent for the same (resource, usage, stage) within one encoder: remember what this render encoder has already declared and skip repeats (a compute encoder is created anew for each dispatch, no dedup)
// Snapshot substitution for same-pass feedback sampling (registered by fb_prepare, used by encode_args; see the notes before draw)
static uint64_t g_draw_seq, g_fb_seq, g_fb_count; static int g_fb_n, g_fb_rebind;
static __unsafe_unretained id<MTLTexture> g_fb_src[4]; static __strong id<MTLTexture> g_fb_use[4]; static uint64_t g_fb_rid[4];
static uint8_t g_args_dirty[6] = {1, 1, 1, 1, 1, 1};
static uint64_t g_args_reg_seen, g_args_dirty_seen;
static inline void args_dirty_all(void) { memset(g_args_dirty, 1, sizeof g_args_dirty); }
static inline void args_sync_dirty(void) {
    uint64_t reg = g_reg_gen, dirty = nvn_dirty_now();
    if (reg != g_args_reg_seen || dirty != g_args_dirty_seen) {
        g_args_reg_seen = reg; g_args_dirty_seen = dirty; args_dirty_all();
    }
}
static void bind_args(id<MTLCommandEncoder> en, int nvn_stage, int metal_vertex, id<MTLBuffer> ab, uint64_t gpu, size_t off, NSUInteger ai) {
    (void)gpu;   // The resolved buffer and offset are already available; avoid the second address-index lookup.
    if (nvn_stage == 5) [(id<MTLComputeCommandEncoder>)en setBuffer:ab offset:off atIndex:ai];
    else legacy_bind_buffer((id<MTLRenderCommandEncoder>)en, !metal_vertex, ai, ab, off);
}
// Static constant buffers of route B: the Ryujinx support_buffer (layout in Ryujinx.Graphics.Shader/SupportBuffer.cs: alpha_test u32, is_bgra u32[8],
// viewport_inverse f4, viewport_size f4, frag_scale_count i32, render_scale f32[73]..., each field aligned to 16 bytes) and the all-0 driver constant segment (64 KB = the upper bound declared by shaders)
static id<MTLBuffer> support_buf(void) {
    static __strong id<MTLBuffer> b; if (b) return b;
    b = [g_dev newBufferWithLength:65536 options:MTLResourceStorageModeShared]; res_add(b);   // All 0 when newly created
    const int render_scale_off = 192;   // Measured MSL layout: alpha_test 0, is_bgra[8] 16, viewport_inverse 144, viewport_size 160, frag_scale_count 176, render_scale float4[73] 192 (take .x)
    for (int i = 0; i < 73; i++) *(float *)((uint8_t *)b.contents + render_scale_off + 16 * i) = 1.0f;
    return b;
}
static uint64_t support_gpu(void) { static uint64_t gpu; if (!gpu) gpu = legacy_buffer_gpu(support_buf()); return gpu; }
static id<MTLBuffer> zero_bank(void) { static __strong id<MTLBuffer> b; if (!b) { b = [g_dev newBufferWithLength:65536 options:MTLResourceStorageModeShared]; res_add(b); } return b; }
static uint64_t zero_bank_gpu(void) { static uint64_t gpu; if (!gpu) gpu = legacy_buffer_gpu(zero_bank()); return gpu; }
// Shift all UI draws down as a whole (iPad true fullscreen, the UI rect does not touch the top edge of the picture): add a clip-space vertical offset to the projection matrix at the start of the vertex stage's cb0, and bind a modified copy instead.
// Moving the viewport is not used: the viewport's top edge is the edge of the clip volume, so top-anchored widgets that the host lifts back to the real top of the picture (UI coordinate y negative) would be clipped away entirely.
// g_ui_moff is set per draw by draw() (0 = no change); the copies live in a ring buffer, and the same address in the same pass is copied only once.
static double g_ui_moff;
static int ui_mat_ok(void) { const p_bind_buf *u = &S.ubo[0][0]; void *mb; uint64_t mo; return u->addr && u->size >= 64 && u->size <= 4096 && !pend_hits(u->addr, u->size) && mtl_addr(u->addr, u->size, &mb, &mo); }
static uint64_t ui_mat_patch(const p_bind_buf *u, void *mb, uint64_t mo) {
    enum { RING = 4 << 20 }; static __strong id<MTLBuffer> ring; static uint64_t ring_gpu, la, ls, lp, lg; static size_t at; static double lo;
    if (!ring) { ring = [g_dev newBufferWithLength:RING options:MTLResourceStorageModeShared]; res_add(ring); ring_gpu = legacy_buffer_gpu(ring); }
    if (la == u->addr && ls == u->size && lp == g_pass.serial && lo == g_ui_moff) return lg;
    const uint8_t *src = [(__bridge id<MTLBuffer>)mb contents]; if (!src || u->size < 64 || u->size > 4096) return 0;
    size_t n = (u->size + 255) & ~(size_t)255; if (at + n > RING) at = 0;
    uint8_t *dst = (uint8_t *)ring.contents + at; memcpy(dst, src + mo, u->size);
    float m[16], o = (float)g_ui_moff; memcpy(m, dst, 64); m[1] += o * m[3]; m[5] += o * m[7]; m[9] += o * m[11]; m[13] += o * m[15]; memcpy(dst, m, 64);
    la = u->addr; ls = u->size; lp = g_pass.serial; lo = g_ui_moff; lg = ring_gpu + at; at += n;
    return lg;
}
// Bindless handle (TEXU / SAMPU): the 32-bit word at byte offset off of uniform slot reg = the low 32 bits of the NVN handle the game wrote there (low 20 bits texture ID, high 12 bits sampler ID).
// Read on the CPU side at replay time (the memory at the same moment a real GPU would read it at execution); slot unbound / out of range / unreadable = 0 (empty slot: dummy texture / dummy sampler).
// A location with a GPU-side write queued in this submit (pend_hits) is still read at its current value; the first case is reported red.
static uint32_t ubo_handle(int stage, int reg, int off) {
    if (stage < 0 || stage >= 6 || reg < 0 || reg >= 16) return 0;
    const p_bind_buf *u = &S.ubo[stage][reg];
    if (!u->addr || (uint64_t)off + 4 > u->size) return 0;
    if (g_nubd && ubd_hits(u->addr + off, u->addr + off + 4) && ubd_covers(u->addr + off, u->addr + off + 4)) { uint32_t h; ubd_read(u->addr + off, 4, &h); return h; }   // written by an inline update deferred in this pass
    if (pend_hits(u->addr + off, 4)) RED_ONCE(47, "无绑定句柄所在的 uniform 缓冲（阶段 %d 槽 %d +%#x）本提交里有 GPU 侧写入：读的是当前值", stage, reg, off);
    void *mb; uint64_t mo; const uint8_t *base;
    if (!mtl_addr(u->addr + off, 4, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) return 0;
    uint32_t h; memcpy(&h, base + mo, 4); return h;
}
// Opt-in Z-A geometry diagnostics. An empty trigger file is inert; changing a
// non-empty file captures one frame. Limits keep live-device logging bounded.
// No pixel reads, synchronization waits or rendering overrides are introduced.
static struct {
    int initialized, frames, samplers, textures, indirect, mismatches;
    const char *file;
    uint64_t probe_frame, frame, token, sampler_seen[64], mismatch_seen[64];
} g_geom_diag;
static const char *geometry_diag_file(void) {
    if (!g_geom_diag.initialized) {
        g_geom_diag.initialized = 1;
        g_geom_diag.probe_frame = g_geom_diag.frame = UINT64_MAX;
        if (DM_GAME_PROGRAM_ID == 0x0100F43008C44000ull)
            g_geom_diag.file = getenv("DM_NVN_GEOMETRY_DIAG_FILE");
        if (g_geom_diag.file && *g_geom_diag.file)
            MLOG("Geometry diagnostics: change non-empty trigger %s; at most 8 frames, 512 indirect records and 64 reduction bindings per frame", g_geom_diag.file);
    }
    return g_geom_diag.file && *g_geom_diag.file ? g_geom_diag.file : NULL;
}
static int geometry_diag_frame(void) {
    const char *file = geometry_diag_file(); if (!file) return 0;
    uint64_t frame = atomic_load(&g_cur_frame);
    if (g_geom_diag.probe_frame != frame && g_geom_diag.frames < 8) {
        g_geom_diag.probe_frame = frame;
        struct stat st;
        if (!stat(file, &st)) {
            uint64_t token = ((uint64_t)st.st_mtimespec.tv_sec * 1000000000ull + (uint64_t)st.st_mtimespec.tv_nsec) ^ (uint64_t)st.st_size;
            if (token != g_geom_diag.token) {
                g_geom_diag.token = token;
                if (st.st_size > 0) {
                    g_geom_diag.frame = frame; g_geom_diag.frames++;
                    g_geom_diag.samplers = g_geom_diag.textures = g_geom_diag.indirect = g_geom_diag.mismatches = 0;
                    MLOG("[geometry] capture=%d frame=%llu", g_geom_diag.frames, (unsigned long long)frame);
                }
            }
        }
    }
    return frame == g_geom_diag.frame;
}
static void geometry_diag_sampler(NvShader *sh, int stage, const nvn_rbind *binding, int sid) {
    if (!geometry_diag_frame() || sid <= 0 || sid >= g_nsreg || !g_sreg_ok[sid] ||
        !g_sreg[sid].reduction || g_geom_diag.samplers >= 64) return;
    uint64_t key = 1469598103934665603ull;
    for (const char *p = sh->key; *p; p++) key = (key ^ (unsigned char)*p) * 1099511628211ull;
    key = (key ^ ((uint64_t)stage << 48 | (uint64_t)binding->kind << 40 | (uint64_t)binding->reg << 24 | (unsigned)sid)) * 1099511628211ull;
    for (int i = 0; i < g_geom_diag.samplers; i++) if (g_geom_diag.sampler_seen[i] == key) return;
    g_geom_diag.sampler_seen[g_geom_diag.samplers++] = key;
    const samp_par *s = &g_sreg[sid];
    uint32_t handle = binding->kind == NVN_RB_SAMPU ? ubo_handle(stage, binding->reg, binding->off) :
                      binding->kind == NVN_RB_SAMPT && binding->reg < NTEX ? (uint32_t)S.tex[stage][binding->reg] : 0;
    int tid = (int)(handle & 0xfffff); treg_t *r = tid > 0 && tid < g_ntreg && g_treg[tid].ok ? &g_treg[tid] : NULL;
    id<MTLTexture> t = r ? (__bridge id<MTLTexture>)r->view : nil;
    MLOG("[geometry-sampler] frame=%llu st=%d key=%s reg=%u kind=%u sid=%d reduction=%u native_reduction=%d minmag=%u/%u lod=%g..%g bias=%g combined_tid=%d serial=%llu nvn_target=%u nvn_format=%#x view=%lux%lux%lu array=%lu mips=%lu type=%lu format=%lu",
         (unsigned long long)g_geom_diag.frame, stage, sh->key, binding->reg, binding->kind, sid, s->reduction,
         [MTLSamplerDescriptor instancesRespondToSelector:NSSelectorFromString(@"setReductionMode:")],
         s->minf, s->magf, s->lod_min, s->lod_max, s->lod_bias, tid, (unsigned long long)(r ? r->t.serial : 0),
         r ? r->t.d.target : 0, r ? r->t.d.format : 0, (unsigned long)t.width, (unsigned long)t.height,
         (unsigned long)t.depth, (unsigned long)t.arrayLength, (unsigned long)t.mipmapLevelCount,
         (unsigned long)t.textureType, (unsigned long)t.pixelFormat);
    // Separate samplers have no inherent paired texture. Record the shader's
    // texture bindings as candidates rather than inventing such a pairing.
    if (!handle) for (int i = 0; i < sh->rf.nb && g_geom_diag.textures < 32; i++) {
        const nvn_rbind *b = &sh->rf.b[i];
        if (b->kind != NVN_RB_TEX && b->kind != NVN_RB_TEXU) continue;
        uint32_t h = b->kind == NVN_RB_TEXU ? ubo_handle(stage, b->reg, b->off) :
                     b->reg < NTEX ? (uint32_t)S.tex[stage][b->reg] : 0;
        int texture_id = (int)(h & 0xfffff);
        treg_t *texture = texture_id > 0 && texture_id < g_ntreg && g_treg[texture_id].ok ? &g_treg[texture_id] : NULL;
        id<MTLTexture> view = texture ? (__bridge id<MTLTexture>)texture->view : nil;
        g_geom_diag.textures++;
        MLOG("[geometry-sampler-texture] frame=%llu st=%d key=%s sampler_reg=%u texture_reg=%u tid=%d serial=%llu nvn_target=%u nvn_format=%#x view=%lux%lux%lu array=%lu mips=%lu type=%lu format=%lu",
             (unsigned long long)g_geom_diag.frame, stage, sh->key, binding->reg, b->reg, texture_id,
             (unsigned long long)(texture ? texture->t.serial : 0), texture ? texture->t.d.target : 0,
             texture ? texture->t.d.format : 0, (unsigned long)view.width, (unsigned long)view.height,
             (unsigned long)view.depth, (unsigned long)view.arrayLength, (unsigned long)view.mipmapLevelCount,
             (unsigned long)view.textureType, (unsigned long)view.pixelFormat);
    }
}
// Report unresolved binding mismatches on demand, even after tex_fit's
// process-wide RED_ONCE has fired. Texture IDs may be reused across scenes.
static void geometry_diag_type_mismatch(NvShader *sh, int stage, const nvn_rbind *b, int tid, id<MTLTexture> t) {
    if (!geometry_diag_frame() || g_geom_diag.mismatches >= 64) return;
    uint64_t hash = 1469598103934665603ull;
    for (const char *p = sh->key; *p; p++) hash = (hash ^ (unsigned char)*p) * 1099511628211ull;
    hash = (hash ^ ((uint64_t)stage << 48 | (uint64_t)b->reg << 32 | (uint64_t)b->dtt << 24 | (uint64_t)t.textureType << 16 | (unsigned)tid)) * 1099511628211ull;
    for (int i = 0; i < g_geom_diag.mismatches; i++) if (g_geom_diag.mismatch_seen[i] == hash) return;
    g_geom_diag.mismatch_seen[g_geom_diag.mismatches++] = hash;
    const treg_t *r = tid > 0 && tid < g_ntreg && g_treg[tid].ok ? &g_treg[tid] : NULL;
    MLOG("[geometry-type-mismatch] frame=%llu pass=%llu st=%d key=%s reg=%u aid=%u kind=%u expected=%u actual=%lu pixel_class=%u tid=%d serial=%llu pool=%p offset=%#llx nvn_target=%u nvn_format=%#x nvn_size=%ux%ux%u nvn_levels=%u has_view=%u view_target=%u/%u view_format=%u/%#x view_levels=%u/%u+%u view_layers=%u/%u+%u metal=%lux%lux%lu array=%lu mips=%lu format=%lu usage=%#lx parent_type=%lu parent_level=%lu parent_slice=%lu",
         (unsigned long long)g_geom_diag.frame, (unsigned long long)g_stat_pass, stage, sh->key, b->reg, b->aid, b->kind,
         b->dtt, (unsigned long)t.textureType, b->dpf, tid, (unsigned long long)(r ? r->t.serial : 0), r ? r->t.pool : NULL,
         (unsigned long long)(r ? r->t.off : 0), r ? r->t.d.target : 0, r ? r->t.d.format : 0,
         r ? r->t.d.width : 0, r ? r->t.d.height : 0, r ? r->t.d.depth : 0, r ? r->t.d.levels : 0,
         r ? r->has_v : 0, r && r->has_v ? r->v.set_target : 0, r && r->has_v ? r->v.target : 0,
         r && r->has_v ? r->v.set_format : 0, r && r->has_v ? r->v.format : 0,
         r && r->has_v ? r->v.set_levels : 0, r && r->has_v ? r->v.base_level : 0, r && r->has_v ? r->v.nlevels : 0,
         r && r->has_v ? r->v.set_layers : 0, r && r->has_v ? r->v.min_layer : 0, r && r->has_v ? r->v.nlayers : 0,
         (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.depth, (unsigned long)t.arrayLength,
         (unsigned long)t.mipmapLevelCount, (unsigned long)t.pixelFormat, (unsigned long)t.usage,
         (unsigned long)t.parentTexture.textureType, (unsigned long)t.parentRelativeLevel, (unsigned long)t.parentRelativeSlice);
}
#include "nvn_pixel_history.h"
// Bounded Wonder/A16 colour diagnostics. Metadata only: no pixel readback, uniform dumps,
// GPU waits or per-draw disk files. Caller holds g_rmu; only runs when fragment arguments rebuild.
// At most 8 snapshots per 600-frame window, one per frame, 96 per process. DM_NVN_COLOR_DIAG=0 disables.
static struct { int initialized, on, total, used; uint64_t window, last, seen[96]; } g_color_diag;
static uint64_t color_mix(uint64_t h, uint64_t v) { return (h ^ v) * 1099511628211ull; }
static int color_diag_begin(NvShader *sh, int stage) {
    if (stage != 1 || DM_GAME_PROGRAM_ID != 0x010015100B514000ull) return 0;
    if (!g_color_diag.initialized) {
        g_color_diag.initialized = 1;
        const char *e = getenv("DM_NVN_COLOR_DIAG");
        g_color_diag.on = e ? strcmp(e, "0") != 0 : no_bc();
        g_color_diag.last = ~0ull;
        if (g_color_diag.on) MLOG("[color] 颜色诊断启用：每600帧最多8组、每帧1组、全程96组；按着色器/格式/尺寸/通道去重，不按瞬时序号；只记绑定元数据；DM_NVN_COLOR_DIAG=0关闭");
    }
    if (!g_color_diag.on || g_color_diag.total >= 96) return 0;
    uint64_t frame = atomic_load(&g_cur_frame), window = frame / 600;
    if (window != g_color_diag.window) { g_color_diag.window = window; g_color_diag.used = 0; }
    if (g_color_diag.used >= 8 || g_color_diag.last == frame) return 0;
    unsigned reasons = 0; uint64_t hash = 1469598103934665603ull;
    for (const char *k = sh->key; *k; k++) hash = color_mix(hash, (unsigned char)*k);
    hash = color_mix(hash, R.rt.n);
    for (int k = 0; k < R.rt.n && k < 8; k++) if (R.rt.color[k]) {
        hash = color_mix(hash, R.rt.color[k]->d.format);
        hash = color_mix(hash, (uint64_t)R.rt.color[k]->d.width << 32 | R.rt.color[k]->d.height);
    }
    for (int i = 0; i < sh->rf.nb; i++) {
        const nvn_rbind *b = &sh->rf.b[i]; int reg = b->reg;
        if (b->kind == NVN_RB_SAMP || b->kind == NVN_RB_SAMPT || b->kind == NVN_RB_SAMPU) {
            int sid = b->kind == NVN_RB_SAMPU ? (int)(ubo_handle(stage, reg, b->off) >> 20) :
                      b->kind == NVN_RB_SAMPT ? (reg < NTEX ? (int)(S.tex[stage][reg] >> 20) : 0) :
                      reg < 128 ? (int)(S.samp[stage][reg] >> 20) : 0;
            if (sid > 0 && sid < g_nsreg && g_sreg_ok[sid]) {
                hash = color_mix(hash, g_sreg[sid].reduction);
                if (g_sreg[sid].reduction) reasons |= 1;
            }
        } else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TEXU) {
            int tid = b->kind == NVN_RB_TEXU ? (int)(ubo_handle(stage, reg, b->off) & 0xfffff) : reg < NTEX ? (int)(S.tex[stage][reg] & 0xfffff) : 0;
            if (tid <= 0 || tid >= g_ntreg || !g_treg[tid].ok) continue;
            const treg_t *t = &g_treg[tid];
            hash = color_mix(hash, t->t.d.format);
            hash = color_mix(hash, (uint64_t)t->t.d.width << 32 | t->t.d.height);
            hash = color_mix(hash, t->t.set_swz ? t->t.swz : (2u | 3u << 3 | 4u << 6 | 5u << 9));
            if (t->has_v && t->v.set_swizzle) for (int c = 0; c < 4; c++) hash = color_mix(hash, t->v.swz[c]);
            if (t->t.d.format == 0x4a || t->t.d.format == 0x4c) reasons |= 2;   // signed BC4/5
            if (t->t.set_swz || (t->has_v && t->v.set_swizzle)) reasons |= 4;
            for (int k = 0; k < R.rt.n && k < 8; k++) if (R.rt.color[k] && R.rt.color[k]->serial == t->t.serial) reasons |= 8;
        }
    }
    if (!reasons) return 0;
    for (int i = 0; i < g_color_diag.total; i++) if (g_color_diag.seen[i] == hash) return 0;
    g_color_diag.seen[g_color_diag.total] = hash; g_color_diag.used++; g_color_diag.total++; g_color_diag.last = frame;
    MLOG("[color] #%d F%llu pass=%llu fs=%s reasons=%u rt=%d depth=%#x fb=%d（1归约/2带符号BC/4通道重排/8附件采样）",
         g_color_diag.total, (unsigned long long)frame, g_stat_pass, sh->key, reasons, R.rt.n,
         R.rt.depth ? R.rt.depth->d.format : 0, g_fb_n && g_fb_seq == g_draw_seq);
    for (int k = 0; k < R.rt.n && k < 8; k++) if (R.rt.color[k])
        MLOG("[color]   rt%d ser=%llu nvn=%#x size=%ux%u", k, (unsigned long long)R.rt.color[k]->serial,
             R.rt.color[k]->d.format, R.rt.color[k]->d.width, R.rt.color[k]->d.height);
    return 1;
}
static uint32_t g_dispatch_dim[3];   // the current dispatch's workgroup counts (driver segment c0 + 0x200 for compute)
static uint32_t g_draw_binst;        // current draw's base instance (driver segment c0 + 0x644)
// An indirect draw's base instance is produced on the GPU, so it is not in the
// CPU-side p_draw record when encode_args runs.  safe_indirect_encode gives the
// draw a private c0 and patches +0x644 from the same guest indirect record.  The
// rest of c0 is still populated here from the replay state before submission.
static __unsafe_unretained id<MTLBuffer> g_indirect_drv0_buf;
static uint64_t g_indirect_drv0_gpu;
static NSUInteger g_indirect_drv0_off;
static int encode_args_(id<MTLCommandEncoder> en, NvShader *sh, int nvn_stage, int metal_vertex);
// DM_NVN_SAMPLE_TRACE=<comma-separated shader key prefixes>: keep binding metadata in memory,
// attached to this submission's first GPU error, without per-draw log I/O/waits.
// Optional DM_NVN_SAMPLE_TRACE_CB=<register>:<hex byte offset> records one float4
// from the actual encoded GPU buffer address (only if CPU-readable and in range).
// DM_NVN_SAMPLE_TRACE_VERTEX=1 also observes vertex arguments. BYTES=1 includes
// bounded CPU-readable constant, vertex and index snapshots; never GPU readback.
static const p_draw_idx *g_sample_trace_di;
static const p_draw *g_sample_trace_dw;
static int sample_trace_key(const char *key, const char *shader) {
    for (const char *p = key; p && *p;) {
        const char *comma = strchr(p, ','); size_t n = comma ? (size_t)(comma - p) : strlen(p);
        if (n && !strncmp(shader, p, n)) return 1;
        p = comma ? comma + 1 : NULL;
    }
    return 0;
}
static void sample_trace_bytes(id<MTLBuffer> buffer, NSUInteger offset, uint64_t bytes) {
    if (!buffer.contents || offset > buffer.length) return;
    NSUInteger n = (NSUInteger)MIN(MIN(bytes, 1024ull), (uint64_t)(buffer.length - offset));
    const uint8_t *p = (const uint8_t *)buffer.contents + offset;
    [g_sub_sample_notes appendFormat:@" bytes=%lu hex=", (unsigned long)n];
    for (NSUInteger j = 0; j < n; j++) [g_sub_sample_notes appendFormat:@"%02x", p[j]];
}
static uint64_t sample_trace_hash(id<MTLBuffer> buffer, NSUInteger offset, NSUInteger length) {
    const uint8_t *p = (const uint8_t *)buffer.contents + offset;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (NSUInteger i = 0; i < length; i++) { hash ^= p[i]; hash *= UINT64_C(1099511628211); }
    return hash;
}
static void sample_trace_root(NvShader *sh, int stage, id<MTLBuffer> buffer, NSUInteger offset, NSUInteger length) {
    static const char *keys; static int init; static unsigned count;
    static unsigned long long first_frame, last_frame = UINT64_MAX;
    if (!init) {
        init = 1; keys = getenv("DM_NVN_SAMPLE_TRACE");
        const char *range = getenv("DM_NVN_SAMPLE_TRACE_ROOT_FRAMES");
        if (range && sscanf(range, "%llu-%llu", &first_frame, &last_frame) != 2) keys = NULL;
    }
    uint64_t frame = atomic_load(&g_cur_frame);
    if (!keys || frame < first_frame || frame > last_frame || !sample_trace_key(keys, sh->key) || count >= 192 || !buffer.contents || offset > buffer.length || length > buffer.length - offset) return;
    count++;
    uint64_t hash = sample_trace_hash(buffer, offset, length), pass = g_pass.serial;
    NSString *key = @(sh->key);
    if (!g_sub_argument_checks) g_sub_argument_checks = [NSMutableArray array];
    [g_sub_argument_checks addObject:^{
        uint64_t completed = sample_trace_hash(buffer, offset, length);
        MLOG("%s[arg-trace] frame=%llu pass=%llu stage=%d shader=%s buffer=%p length=%lu offset=%lu encoded=%lu alignment=%lu before=%016llx completed=%016llx unchanged=%d",
            hash == completed ? "" : "[RED] ", (unsigned long long)frame, (unsigned long long)pass, stage, key.UTF8String,
            (__bridge void *)buffer, (unsigned long)buffer.length, (unsigned long)offset, (unsigned long)length,
            (unsigned long)sh->ae.alignment, (unsigned long long)hash, (unsigned long long)completed, hash == completed);
    }];
}
static void sample_trace_args(NvShader *sh, int stage, const uint64_t *slots) {
    static const char *key; static int init, cbreg = -1, vertex, bytes; static unsigned long long cboff;
    if (!init) { init = 1; key = getenv("DM_NVN_SAMPLE_TRACE"); const char *c = getenv("DM_NVN_SAMPLE_TRACE_CB"); if (c && sscanf(c, "%d:%llx", &cbreg, &cboff) != 2) cbreg = -1;
        const char *v = getenv("DM_NVN_SAMPLE_TRACE_VERTEX"), *b = getenv("DM_NVN_SAMPLE_TRACE_BYTES"); vertex = v && atoi(v); bytes = b && atoi(b); }
    if ((stage != 1 && !(stage == 0 && vertex)) || !key || !*key || !sample_trace_key(key, sh->key)) return;
    if (!g_sub_sample_notes) g_sub_sample_notes = [NSMutableString string];
    if (g_sub_sample_notes.length >= (bytes ? 262144 : 24576)) return;
    [g_sub_sample_notes appendFormat:@"frame=%llu pass=%llu stage=%d shader=%s target=%ux%u fmt=%u ae=%lu/%lu\n", (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)g_pass.serial, stage, sh->key, R.rt.n && R.rt.color[0] ? R.rt.color[0]->d.width : 0, R.rt.n && R.rt.color[0] ? R.rt.color[0]->d.height : 0, g_pass.cfmt[0], (unsigned long)sh->ae.encodedLength, (unsigned long)sh->ae.alignment];
    if (stage == 0 && bytes) {
        for (unsigned stream = 0; stream < 16; stream++) if (S.vbuf[stream].address && S.vbuf[stream].size) {
            void *mb; uint64_t off;
            [g_sub_sample_notes appendFormat:@"  vertex-stream=%u address=%#llx size=%llu stride=%u divisor=%u", stream, (unsigned long long)S.vbuf[stream].address, (unsigned long long)S.vbuf[stream].size, S.stream[stream].stride, S.stream[stream].divisor];
            if (!pend_hits(S.vbuf[stream].address, MIN(S.vbuf[stream].size, 1024ull)) && mtl_addr(S.vbuf[stream].address, 1, &mb, &off)) sample_trace_bytes((__bridge id<MTLBuffer>)mb, off, S.vbuf[stream].size);
            [g_sub_sample_notes appendString:@"\n"];
        }
        if (g_sample_trace_di) {
            const p_draw_idx *di = g_sample_trace_di; void *mb; uint64_t off;
            uint64_t n = di->count > 0 && di->itype >= 0 && di->itype <= 2 ? (uint64_t)di->count << di->itype : 0;
            [g_sub_sample_notes appendFormat:@"  indices=%#llx type=%d count=%d base-vertex=%d base-instance=%d indirect=%#llx", (unsigned long long)di->ibuf, di->itype, di->count, di->base_vertex, di->base_instance, (unsigned long long)di->ind];
            if (n && !pend_hits(di->ibuf, MIN(n, 1024ull)) && mtl_addr(di->ibuf, 1, &mb, &off)) sample_trace_bytes((__bridge id<MTLBuffer>)mb, off, n);
            [g_sub_sample_notes appendString:@"\n"];
        } else if (g_sample_trace_dw) [g_sub_sample_notes appendFormat:@"  arrays first=%d count=%d base-instance=%d\n", g_sample_trace_dw->first, g_sample_trace_dw->count, g_sample_trace_dw->base_instance];
    }
    for (int i = 0; i < sh->rf.nb; i++) {
        const nvn_rbind *b = &sh->rf.b[i]; unsigned reg = b->reg;
        [g_sub_sample_notes appendFormat:@"  kind=%u reg=%u aid=%u encoded=%#llx", b->kind, reg, b->aid, (unsigned long long)slots[b->aid]];
        if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TEXU) {
            int tid = b->kind == NVN_RB_TEXU ? (int)(ubo_handle(stage, reg, b->off) & 0xfffff) : reg < NTEX ? S.tex[stage][reg] & 0xfffff : 0;
            if (tid > 0 && tid < g_ntreg && g_treg[tid].ok) {
                treg_t *r = &g_treg[tid]; id<MTLTexture> t = (__bridge id<MTLTexture>)r->view;
                [g_sub_sample_notes appendFormat:@" tid=%d ser=%llu pool=%p off=%#llx view=%p size=%lux%lu mips=%lu fmt=%lu type=%lu usage=%#lx parent=%p level=%lu", tid, (unsigned long long)r->t.serial, r->t.pool, (unsigned long long)r->t.off, (__bridge void *)t, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.mipmapLevelCount, (unsigned long)t.pixelFormat, (unsigned long)t.textureType, (unsigned long)t.usage, (__bridge void *)t.parentTexture, (unsigned long)t.parentRelativeLevel];
            }
        } else if (b->kind == NVN_RB_SAMP || b->kind == NVN_RB_SAMPT || b->kind == NVN_RB_SAMPU) {
            int sid = b->kind == NVN_RB_SAMPU ? ubo_handle(stage, reg, b->off) >> 20 : b->kind == NVN_RB_SAMPT ? (reg < NTEX ? S.tex[stage][reg] >> 20 : 0) : reg < 128 ? S.samp[stage][reg] >> 20 : 0;
            if (sid > 0 && sid < g_nsreg && g_sreg_ok[sid]) { samp_par p = g_sreg[sid]; [g_sub_sample_notes appendFormat:@" sid=%d lod=%g..%g bias=%g minmag=%u/%u refs=%lu", sid, p.lod_min, p.lod_max, p.lod_bias, p.minf, p.magf, (unsigned long)g_sub_samplers.count]; }
        } else if (b->kind == NVN_RB_C1) {
            NSUInteger off; id<MTLBuffer> buffer = legacy_buffer_at(slots[b->aid], &off);
            [g_sub_sample_notes appendFormat:@" c1-size=%u buffer=%p length=%lu offset=%lu cpu-readable=%d", sh->rf.c1_len, (__bridge void *)buffer, (unsigned long)buffer.length, (unsigned long)off, buffer.contents != NULL];
            if (bytes) sample_trace_bytes(buffer, off, sh->rf.c1_len);
        } else if (b->kind == NVN_RB_CBUF && reg < 16 && (bytes || reg == cbreg)) {
            NSUInteger off; id<MTLBuffer> buffer = legacy_buffer_at(slots[b->aid], &off);
            [g_sub_sample_notes appendFormat:@" bound-size=%llu buffer=%p length=%lu offset=%lu cpu-readable=%d", (unsigned long long)S.ubo[stage][reg].size, (__bridge void *)buffer, (unsigned long)buffer.length, (unsigned long)off, buffer.contents != NULL];
            if (bytes && !pend_hits(S.ubo[stage][reg].addr, MIN(S.ubo[stage][reg].size, 1024ull))) sample_trace_bytes(buffer, off, S.ubo[stage][reg].size);
            if (buffer.contents && cboff <= S.ubo[stage][reg].size && 16 <= S.ubo[stage][reg].size - cboff && off <= buffer.length && cboff <= buffer.length - off && 16 <= buffer.length - off - cboff) {
                float v[4]; memcpy(v, (const uint8_t *)buffer.contents + off + cboff, sizeof v);
                [g_sub_sample_notes appendFormat:@" float4+%#llx=%g,%g,%g,%g buffer=%p offset=%lu", cboff, v[0], v[1], v[2], v[3], (__bridge void *)buffer, (unsigned long)off];
            }
        }
        [g_sub_sample_notes appendString:@"\n"];
    }
}
static int encode_args(id<MTLCommandEncoder> en, NvShader *sh, int nvn_stage, int metal_vertex) { AP_B; int r = encode_args_(en, sh, nvn_stage, metal_vertex); AP_E(3); return r; }
static int encode_args_(id<MTLCommandEncoder> en, NvShader *sh, int nvn_stage, int metal_vertex) {
    if (!sh->ae) { sh->min_reduction_bound = 0; return 1; }
    args_sync_dirty();
    if (!passdump() && !g_args_dirty[nvn_stage] && sh->ab_buf && sh->ab_pass == g_pass.serial && nvn_stage != 5 && !(sh->rf.drv_read[0] && sh->ab_binst != g_draw_binst)) {
        bind_args(en, nvn_stage, metal_vertex, sh->ab_buf, sh->ab_gpu, sh->ab_off, sh->rf.abi);
        g_stat_args_reuse++;
        return 1;
    }
    NSUInteger arg_len = sh->arg_len ? sh->arg_len : sh->ae.encodedLength;
    uint64_t slots[NVN_REFL_MAXARG] = {0};
    #define USE(r, u) do { legacy_use_resource(en, (id<MTLResource>)(r), (u), nvn_stage == 5); if (__builtin_expect(g_res_check_on, 0)) res_check((r), sh->key, reg); } while (0)
    // DM_NVN_PASSDUMP: on the first new encode per pass per stage, records the shader key (K) and the actual source of each binding (B: UBO address / length / first 4 floats, texture ID and serial, or 「哑」(dummy))
    static uint64_t bl_pass[6]; static int bl_all = -1; if (bl_all < 0) bl_all = getenv("DM_NVN_PASSDUMP_ALL") != NULL;   // Record on every new encode (otherwise only the first time per pass per stage)
    int bl = passdump() && (bl_all || g_kd_force || bl_pass[nvn_stage] != g_stat_pass);
    if (bl) { bl_pass[nvn_stage] = g_stat_pass; PD("K %llu st%d key=%s nb=%d", g_stat_pass, nvn_stage, sh->key[0] ? sh->key : sh->builtin ? sh->builtin : "?", sh->rf.nb); }
    pthread_mutex_lock(&g_rmu);   // The texture / sampler registration tables may be being grown or rewritten by the game thread
    int min_candidate = min_reduction_candidate(sh); min_reduction_binding min_binding = {0};
    int color_diag = color_diag_begin(sh, nvn_stage), color_lines = 0;
    for (int i = 0; i < sh->rf.nb; i++) {
        const nvn_rbind *b = &sh->rf.b[i];
        int reg = b->reg, aid = b->aid;
        if (b->kind == NVN_RB_CBUF) {
            const p_bind_buf *u = reg < 16 ? &S.ubo[nvn_stage][reg] : NULL;
            void *mb; uint64_t mo, gpu;
            if (!u || !u->addr || !mtl_gpu_addr(u->addr, 1, &mb, &mo, &gpu)) { RED_ONCE(16, "阶段 %d cb%d 没有绑定 uniform 缓冲", nvn_stage, reg); continue; }
            use_rec("buf", u->addr, u->size, 0, NULL);
            if (g_nubd && ubd_hits(u->addr, u->addr + u->size)) {   // inline updates deferred in this pass (see mtl_sub_update); ubd_conflict made sure they cover the binding
                if (ubd_covers(u->addr, u->addr + u->size)) {
                    gpu = ubd_shadow(u->addr, u->addr + u->size);
                    // Argument-buffer pointees need an explicit useResource on Metal 3.
                    NSUInteger shadow_off; id<MTLBuffer> shadow = legacy_buffer_at(gpu, &shadow_off);
                    if (!shadow) MDIE("推迟 uniform 的影子缓冲未登记");
                    USE(shadow, MTLResourceUsageRead);
                }
                else RED_ONCE(92, "通道内推迟的 uniform 更新只盖住绑定的一部分（阶段 %d cb%d）：读的是更新前的内容", nvn_stage, reg); }
            if (nvn_stage == 0 && reg == 0 && g_ui_moff != 0) { uint64_t pg = ui_mat_patch(u, mb, mo); if (pg) gpu = pg; }
            if (bl) {
                PD("B %llu st%d cb reg%d addr=%#llx size=%llu pool=%#llx+%#llx f=%g,%g,%g,%g", g_stat_pass, nvn_stage, reg, (unsigned long long)u->addr, (unsigned long long)u->size,
                   (unsigned long long)(u->addr - mo), (unsigned long long)mo, ((float *)u->addr)[0], ((float *)u->addr)[1], ((float *)u->addr)[2], ((float *)u->addr)[3]);
                // DM_NVN_CBDUMP=directory: also saves the whole uniform block to disk as-is (F<frame>_p<pass>_st<stage>_cb<slot>_<address>.bin), for the ground-truth side to compare byte by byte by guest address
                // DM_NVN_CBDUMP_KEY=shader key: only save for draws using that shader (any stage)
                static const char *cbd, *cbk; static int cbd_init; if (!cbd_init) { cbd = getenv("DM_NVN_CBDUMP"); cbk = getenv("DM_NVN_CBDUMP_KEY"); cbd_init = 1; }
                if (cbd && (!cbk || !strcmp(sh->key, cbk))) {
                    char p[1024]; snprintf(p, sizeof p, "%s/F%llu_p%llu_st%d_cb%d_%llx.bin", cbd, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass, nvn_stage, reg, (unsigned long long)u->addr);
                    FILE *f = fopen(p, "wb"); if (f) { fwrite((const void *)u->addr, 1, u->size, f); fclose(f); } else RED_ONCE(41, "CBDUMP 写不出 %s", p);
                }
            }
            slots[aid] = gpu;
            USE((__bridge id<MTLBuffer>)mb, MTLResourceUsageRead);
            // ui_mat_patch and deferred updates can replace the original pool.
            // Declare the final indirect pointee as well as its source storage.
            NSUInteger final_offset; id<MTLBuffer> final_buffer = legacy_buffer_at(gpu, &final_offset);
            if (final_buffer != (__bridge id<MTLBuffer>)mb) USE(final_buffer, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_SAMP || b->kind == NVN_RB_SAMPT || b->kind == NVN_RB_SAMPU) {   // SAMPT: the sampler part of a combined handle (texture ID | sampler ID << 20); SAMPU: the same, with the handle in a uniform buffer
            int sid = b->kind == NVN_RB_SAMPU ? (int)(ubo_handle(nvn_stage, reg, b->off) >> 20) :
                      b->kind == NVN_RB_SAMPT ? (reg < NTEX ? (int)(S.tex[nvn_stage][reg] >> 20) : 0) :
                      reg < 128 ? (int)(S.samp[nvn_stage][reg] >> 20) : 0;
            __unsafe_unretained id<MTLSamplerState> ss = sid ? (__bridge id<MTLSamplerState>)samp_fast(sid) : nil;
            if (ss) [g_sub_samplers addObject:ss]; // useResource cannot retain non-MTLResource sampler states.
            slots[aid] = sid && sid < g_nsreg && g_srid[sid] ? g_srid[sid] : dummy_samp_rid();
            if (min_candidate) min_reduction_bind_sampler(&min_binding, b, sid, sid > 0 && sid < g_nsreg && g_sreg_ok[sid] ? &g_sreg[sid] : NULL, ss != nil);
            geometry_diag_sampler(sh, nvn_stage, b, sid);
            if (color_diag && color_lines < 16 && sid > 0 && sid < g_nsreg && g_sreg_ok[sid]) {
                const samp_par *sp = &g_sreg[sid]; color_lines++;
                MLOG("[color]   samp reg=%d sid=%d requested_reduction=%u actual=average minmag=%u/%u lod=%g..%g bias=%g compare=%u/%u",
                     reg, sid, sp->reduction, sp->minf, sp->magf, sp->lod_min, sp->lod_max, sp->lod_bias, sp->cmp_mode, sp->cmp_func);
            }
            // Samplers are not MTLResource objects and require no useResource declaration.
            if (bl && sid > 0) { int ok = sid < g_nsreg && g_sreg_ok[sid]; samp_par sp = ok ? g_sreg[sid] : (samp_par){0};   // g_rmu is already held in this section
                if (ok) PD("B %llu st%d samp reg%d sid=%d mag=%d min=%d（0 最近点 / 1 双线性；2–5 带 mip）wrap=%d,%d,%d border=%g,%g,%g,%g lod=%g..%g bias=%g aniso=%g cmp=%d/%d %s", g_stat_pass, nvn_stage, reg, sid, (int)sp.magf, (int)sp.minf,
                           (int)sp.ws, (int)sp.wt, (int)sp.wr, sp.border[0], sp.border[1], sp.border[2], sp.border[3], sp.lod_min, sp.lod_max, sp.lod_bias, sp.aniso, (int)sp.cmp_mode, (int)sp.cmp_func, ss ? "实" : "建不出"); }
        } else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TEXU) {
            int tid = b->kind == NVN_RB_TEXU ? (int)(ubo_handle(nvn_stage, reg, b->off) & 0xfffff) : reg < NTEX ? (int)(S.tex[nvn_stage][reg] & 0xfffff) : 0;
            void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
            za_diag_use("sample", sh, nvn_stage, reg, tid, t && tid < g_ntreg ? g_treg[tid].t.serial : 0);
            if (t && g_fb_n && g_fb_seq == g_draw_seq && nvn_stage == 1) for (int q = 0; q < g_fb_n; q++) if (t == g_fb_src[q]) { t = g_fb_use[q]; trid = g_fb_rid[q]; break; }   // same-pass feedback sampling: read the snapshot instead (see fb_prepare)
            if (t && tid < g_ntreg) use_rec("tex", (uint64_t)g_treg[tid].t.mem, g_treg[tid].t.L.size, tid, &g_treg[tid].t);
            if (t && passdump() && tid < g_ntreg) PD("R %llu st%d ser=%llu", g_stat_pass, nvn_stage, g_treg[tid].t.serial);
            if (bl) PD("B %llu st%d tex reg%d tid=%d %s ser=%llu fmt=%lu %lux%lux%lu view_lv=%lu+%lu view_sl=%lu array=%lu target=%lu swz=%d%d%d%d mtl=%p", g_stat_pass, nvn_stage, reg, tid, t ? "实" : "哑", t && tid < g_ntreg ? g_treg[tid].t.serial : 0ull,
                       (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.depth,
                       (unsigned long)(t.parentTexture ? t.parentRelativeLevel : 0), (unsigned long)t.mipmapLevelCount,
                       (unsigned long)(t.parentTexture ? t.parentRelativeSlice : 0), (unsigned long)t.arrayLength, (unsigned long)t.textureType, t ? (int)t.swizzle.red : 0, t ? (int)t.swizzle.green : 0, t ? (int)t.swizzle.blue : 0, t ? (int)t.swizzle.alpha : 0, (__bridge void *)(t.parentTexture ? t.parentTexture : t));
            if (t && t.textureType != (MTLTextureType)b->dtt) { uint64_t r2 = 0; id<MTLTexture> v = tex_fit(tid, t, (MTLTextureType)b->dtt, &r2); if (v) { t = v; trid = r2; } else geometry_diag_type_mismatch(sh, nvn_stage, b, tid, t); }
            if (t && nvn_stage == 1 && afr_want(sh)) { uint64_t r2 = 0; id<MTLTexture> v = tex_alpha_red(tid, t, &r2); if (v) { t = v; trid = r2; } }   // see tex_alpha_red
            if (color_diag && color_lines < 16) {
                const treg_t *tr = tid > 0 && tid < g_ntreg && g_treg[tid].ok ? &g_treg[tid] : NULL;
                color_lines++;
                MLOG("[color]   tex reg=%d tid=%d ser=%llu nvn=%#x requested_view=%#x metal=%lu size=%lux%lu type=%lu/%u levels=%lu+%lu slice=%lu swz=%u,%u,%u,%u dummy=%d",
                     reg, tid, tr ? (unsigned long long)tr->t.serial : 0, tr ? tr->t.d.format : 0,
                     tr && tr->has_v && tr->v.set_format ? tr->v.format : 0, (unsigned long)t.pixelFormat,
                     (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.textureType, b->dtt,
                     (unsigned long)(t.parentTexture ? t.parentRelativeLevel : 0), (unsigned long)t.mipmapLevelCount,
                     (unsigned long)(t.parentTexture ? t.parentRelativeSlice : 0),
                     t ? (unsigned)t.swizzle.red : 0, t ? (unsigned)t.swizzle.green : 0, t ? (unsigned)t.swizzle.blue : 0, t ? (unsigned)t.swizzle.alpha : 0, t == nil);
            }
            if (min_candidate) min_reduction_bind_texture(&min_binding, b, tid, t);
            if (!t) { t = dummy_tex(b->dtt, b->dpf); trid = g_dummy_tex_rid[b->dtt & 7][b->dpf & 3]; }
            slots[aid] = trid;
            USE(t, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_IMG) {   // Writable texture: NVN image slot r (BindImages) ↔ D3D u_r
            int tid = reg < 32 ? (int)(S.img[nvn_stage][reg] & 0xfffff) : 0;
            void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
            za_diag_use("image", sh, nvn_stage, reg, tid, t && tid < g_ntreg ? g_treg[tid].t.serial : 0);
            if (passdump()) PD("W %llu st%d reg%d tid=%d %s ser=%llu", g_stat_pass, nvn_stage, reg, tid, t ? "实" : "哑", t && tid < g_ntreg ? g_treg[tid].t.serial : 0ull);
            if (t && !(t.usage & MTLTextureUsageShaderWrite)) { RED_ONCE(32, "图像槽绑的贴图没有可写用途（贴图号 %d）", tid); t = nil; }
            if (!t) { t = dummy_img(b->dtt, b->dpf); trid = g_dummy_img_rid[b->dtt & 7][b->dpf & 3]; }   // Unbound: D3D semantics discard the write - write into a dedicated dummy texture so the set that reads 0 is not polluted
            slots[aid] = trid;
            USE(t, MTLResourceUsageRead | MTLResourceUsageWrite);
        } else if (b->kind == NVN_RB_IMGA) {   // Image atomics: bind the pool storage behind image slot r as a device pointer.
            int tid = reg < 32 ? (int)(S.img[nvn_stage][reg] & 0xfffff) : 0;
            if (sh->zero_image_variant) {
                if (sh != g_zero_image_shader || !g_zero_image_buffer || nvn_stage != 1 ||
                    aid != sh->zero_image_aid || tid != g_zero_image_tid ||
                    tid <= 0 || tid >= g_ntreg || !g_treg[tid].ok || g_treg[tid].t.serial != g_zero_image_serial ||
                    g_treg[tid].view != (__bridge void *)g_zero_image_texture)
                    MDIE("[RED] Zero-image destination changed between shadow capture and argument encoding");
                slots[aid] = legacy_buffer_gpu(g_zero_image_buffer);
                USE(g_zero_image_buffer, MTLResourceUsageRead | MTLResourceUsageWrite);
                continue;
            }
            const tex_t *tt = tid > 0 && tid < g_ntreg && g_treg[tid].ok ? &g_treg[tid].t : NULL;
            void *mb = NULL; uint64_t mo = 0, gpu = 0;
            // A linear image and its Metal texture view share the same pool
            // words. Block-linear images use a deswizzled private copy, so their
            // atomics cannot safely target the guest layout.
            int linear = tt && (tt->d.target == 10 || (tt->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)));
            int za_shadow = tt && !linear && !strcmp(sh->key, "2bd5037003980716_b");
            if (tt && !linear && !za_shadow) RED_ONCE(93, "image atomics on a block-linear texture (texture ID %d, target %d): writing to a dummy buffer", tid, (int)tt->d.target);
            if (linear && tt->mem && mtl_gpu_addr((uint64_t)tt->mem, tt->L.size ? tt->L.size : 4, &mb, &mo, &gpu)) {
                slots[aid] = gpu;
                USE((__bridge id<MTLBuffer>)mb, MTLResourceUsageRead | MTLResourceUsageWrite);
                static int nlog;
                if (nlog < 8 && !(sh->logged & 2)) {
                    sh->logged |= 2; nlog++;
                    MLOG("shader %s (stage %d): image atomics on image slot %d, texture ID %d (target %d, %llu bytes)", sh->key, nvn_stage, reg, tid, (int)tt->d.target, (unsigned long long)tt->L.size);
                }
            } else {
                size_t need = 64u << 10;
                if (tt) {
                    size_t w = MAX((size_t)tt->d.width, 1), h = MAX((size_t)tt->d.height, 1), d = MAX((size_t)tt->d.depth, 1);
                    size_t logical = w <= SIZE_MAX / h ? w * h : SIZE_MAX;
                    logical = logical <= SIZE_MAX / d ? logical * d : SIZE_MAX;
                    logical = logical <= SIZE_MAX / 4 ? logical * 4 : SIZE_MAX;
                    need = MAX((size_t)tt->L.size, logical);
                }
                id<MTLBuffer> sink = za_shadow ? za_atomic_shadow(need) : nil;
                int using_shadow = sink != nil;
                if (!sink) sink = imga_dummy(need);
                slots[aid] = legacy_buffer_gpu(sink);
                USE(sink, MTLResourceUsageRead | MTLResourceUsageWrite);
                if (using_shadow) {
                    void *tv = NULL; (void)tex_fast_rid(tid, &tv);
                    za_atomic_schedule(sink, (__bridge id<MTLTexture>)tv);
                }
            }
            if (bl) PD("B %llu st%d imga reg%d tid=%d %s", g_stat_pass, nvn_stage, reg, tid, linear && tt->mem ? "real" : "dummy");
        } else if (b->kind == NVN_RB_TBUF) {   // Typed buffer: the buffer texture (target BUFFER) in texture slot r
            int tid = reg < NTEX ? (int)(S.tex[nvn_stage][reg] & 0xfffff) : 0;
            void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
            if (t && t.textureType != MTLTextureTypeTextureBuffer) { RED_ONCE(40, "类型化缓冲槽绑的不是缓冲贴图（贴图号 %d 类型 %lu）", tid, (unsigned long)t.textureType); t = nil; }
            if (bl) PD("B %llu st%d tbuf reg%d tid=%d %s ser=%llu", g_stat_pass, nvn_stage, reg, tid, t ? "实" : "哑", t && tid < g_ntreg ? g_treg[tid].t.serial : 0ull);
            if (!t) { t = dummy_tbuf(b->dpf); trid = g_dummy_tbuf_rid[b->dpf & 3]; }
            slots[aid] = trid;
            USE(t, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_SBUFP || b->kind == NVN_RB_UBOP) {   // B: storage buffers / compute-stage uniforms are device pointers; the address is taken by NVN slot
            const p_bind_buf *u = reg < 16 ? (b->kind == NVN_RB_UBOP ? &S.ubo[nvn_stage][reg] : &S.ssbo[nvn_stage][reg]) : NULL;
            void *mb; uint64_t mo, gpu;
            if (u && u->addr && mtl_gpu_addr(u->addr, u->size ? u->size : 1, &mb, &mo, &gpu)) { use_rec("buf", u->addr, u->size, 0, NULL); slots[aid] = gpu; USE((__bridge id<MTLBuffer>)mb, b->rw ? MTLResourceUsageRead | MTLResourceUsageWrite : MTLResourceUsageRead); if (nvn_stage == 5 && b->kind == NVN_RB_SBUFP) pend_add(u->addr, u->size ? u->size : ((__bridge id<MTLBuffer>)mb).length - mo); }
            else { slots[aid] = dummy_buf_gpu(); USE(dummy_buf(), MTLResourceUsageRead); }
            if (bl) PD("B %llu st%d sbufp reg%d %s addr=%#llx size=%llu %s", g_stat_pass, nvn_stage, reg, u && u->addr ? "实" : "哑", u ? (unsigned long long)u->addr : 0ull, u ? (unsigned long long)u->size : 0ull, b->rw ? "rw" : "ro");
        } else if (b->kind == NVN_RB_C1) {      // B: the shader's own constant segment, built when the shader is fetched
            id<MTLBuffer> cb = sh->c1buf ? sh->c1buf : zero_bank();   // c1 has length 0 (600 of them in the whole library) yet the shader still declares it: reads 0
            slots[aid] = (sh->c1buf ? sh->c1_gpu : zero_bank_gpu()) + (sh->c1buf ? sh->c1off : 0);
            USE(cb, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_SUPPORT) {   // support_buffer: scale 1.0, BGRA swap 0, viewport transform not disabled (viewport fields are not read)
            slots[aid] = support_gpu();
            USE(support_buf(), MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_DRV0 || b->kind == NVN_RB_DRV2) {
            // NVN driver constant segments: those proven unread by static analysis at packaging time are supplied as 0; those read are filled on the spot from the current bindings - c0 = storage buffer table (base + 0x10 r: address 64 bits, size 32 bits),
            // c2 = texture handle table (0x168 + 8 r) and sampler handle table (0x568 + 8 r), the values being the NVN handles the game gave at bind time; positions read outside the tables are registered at packaging time and reported a failure at load time
            int bank = b->kind == NVN_RB_DRV2 ? 2 : 0;
            if (!sh->rf.drv_read[bank]) { slots[aid] = zero_bank_gpu(); USE(zero_bank(), MTLResourceUsageRead); }
            else {
                int gpu_binst = bank == 0 && nvn_stage == 0 && g_indirect_drv0_buf != nil;
                id<MTLBuffer> db; size_t doff;
                if (gpu_binst) { db = g_indirect_drv0_buf; doff = g_indirect_drv0_off; }
                else doff = arena_alloc(DRV_SIZE, &db);
                uint8_t *d = (uint8_t *)db.contents + doff;
                // A lookahead conversion can already be running after sub_cut.
                // Preserve the GPU-owned word while populating this draw's c0.
                if (gpu_binst) { memset(d, 0, 0x644); memset(d + 0x648, 0, DRV_SIZE - 0x648); }
                else memset(d, 0, DRV_SIZE);
                if (bank == 0) {
                    int base = c0_ssbo_base(nvn_stage);
                    if (base < 0) RED_ONCE(45, "阶段 %d 的着色器读驱动段 c0，存储缓冲表基址未实测", nvn_stage);
                    else for (int r = 0; r < 16; r++) { uint64_t a = S.ssbo[nvn_stage][r].addr; uint32_t n = (uint32_t)S.ssbo[nvn_stage][r].size; memcpy(d + base + 16 * r, &a, 8); memcpy(d + base + 16 * r + 8, &n, 4); }
                    // Compute also has the dispatch size at 0x200 (x, y, z) and the uniform buffer table at 0x210 + 16 r (address 64 bits, size 32 bits), ending where the storage buffer table
                    // starts (0x310). Measured on Link's Awakening's light culling (light_culling_compute.bfsha): tile index = workgroup y × c0[0x200] + workgroup x, and c0[0x280] is
                    // the address of uniform buffer 7 (the light parameters, 144 bytes per light); Breath of the Wild's compute shaders read 0x280 / 0x290 / 0x2d0 of the same table
                    // Graphics driver constant: maximum point size (upstream ca7216ceb89c).
                    if (nvn_stage != 5) { const float max_point = 2047.0f; memcpy(d + 0x610, &max_point, 4); if (!gpu_binst) memcpy(d + 0x644, &g_draw_binst, 4); }
                    if (nvn_stage == 5) {
                        memcpy(d + 0x200, g_dispatch_dim, 12);
                        for (int r = 0; r < 16; r++) { uint64_t a = S.ubo[5][r].addr; uint32_t n = (uint32_t)S.ubo[5][r].size; memcpy(d + 0x210 + 16 * r, &a, 8); memcpy(d + 0x210 + 16 * r + 8, &n, 4); }
                    }
                } else {
                    const uint64_t *tx = S.tex[nvn_stage], *im = S.img[nvn_stage];
                    // Combined slots 32-40 overlap the image words. A bound
                    // image wins at the overlapping driver offset.
                    for (int r = 0; r < C2_TEX_SLOTS; r++) {
                        if (r >= 32 && (!tx[r] || (r - 32 < C2_IMG_SLOTS && im[r - 32]))) continue;
                        uint32_t th = (uint32_t)tx[r]; memcpy(d + C2_TEX + 8 * r, &th, 4);
                    }
                    for (int r = 0; r < C2_IMG_SLOTS; r++) {
                        uint32_t ih = (uint32_t)im[r]; memcpy(d + C2_IMG + 8 * r, &ih, 4);
                    }
                    int sep_bound = 0;
                    for (int r = 0; r < 128; r++) {
                        uint32_t th = (uint32_t)tx[NVN_SEPTEX_SLOT0 + r];
                        uint32_t sh_ = (uint32_t)S.samp[nvn_stage][r];
                        sep_bound += th != 0;
                        memcpy(d + C2_SEPTEX + 8 * r, &th, 4);
                        memcpy(d + C2_SAMP + 8 * r, &sh_, 4);
                    }
                    if (sep_bound) { static int once; if (!once++) MLOG("unified NVN driver c2: populated %d separate texture handles for stage %d", sep_bound, nvn_stage); }
                }
                slots[aid] = gpu_binst ? g_indirect_drv0_gpu : g_cur_arena->gpu + doff;
                USE(db, MTLResourceUsageRead);
            }
        } else RED_ONCE(46, "绑定种类 %d 未实现", b->kind);
    }
    sample_trace_args(sh, nvn_stage, slots);
    if (min_candidate) min_reduction_binding_finish(sh, &min_binding);
    #undef USE
    pthread_mutex_unlock(&g_rmu);
    NSUInteger argument_alignment = g_legacy_direct_arguments ? 256 : MAX((NSUInteger)256, sh->ae.alignment);
    if (argument_alignment > SIZE_MAX - arg_len) MDIE("[RED] Shader %s argument allocation overflows", sh->key);
    id<MTLBuffer> ab; size_t off = arena_alloc(arg_len + (argument_alignment > 256 ? argument_alignment - 1 : 0), &ab);
    off += (argument_alignment - off % argument_alignment) % argument_alignment;
    if (g_legacy_direct_arguments) {
        if (arg_len > sizeof slots) MDIE("[RED] Shader %s argument block exceeds slot capacity", sh->key);
        memcpy((uint8_t *)ab.contents + off, slots, arg_len);
    } else {
        char failure[256];
        if (!nvn_argument_encode(sh->ae, ab, off, &sh->rf, slots, NVN_REFL_MAXARG, failure, sizeof failure)) {
            MLOG("[RED] Shader %s stage %d private argument encoding failed: %s", sh->key, nvn_stage, failure);
            return 0;
        }
    }
    bind_args(en, nvn_stage, metal_vertex, ab, g_cur_arena->gpu + off, off, sh->rf.abi);
    sample_trace_root(sh, nvn_stage, ab, off, arg_len);
    sh->ab_buf = ab; sh->ab_gpu = g_cur_arena->gpu + off; sh->ab_off = off; sh->ab_pass = g_pass.serial; sh->ab_binst = g_draw_binst; g_args_dirty[nvn_stage] = 0;
    g_stat_args_enc++;
    return 1;
}
static id<MTLBuffer> g_quad_ib; static uint64_t g_quad_gpu;
static id<MTLBuffer> g_fan_ib; static uint64_t g_fan_gpu;   // non-indexed triangle fans: shared indices (0, i, i+1), base vertex = first
static id<MTLRenderCommandEncoder> rp_begin(void);
// ---------- Pass lookahead: does this pass use depth / stencil ----------
// When opening a pass, scan forward from the op after the current draw, simulating the changes of depth-stencil state, up to where this pass must end (target reset, copy, compute dispatch -
// matching one-to-one the ops that end a pass in the replay). If no draw in between enables depth test / depth write / stencil test, this pass may leave the depth attachment unattached:
// not one byte of the depth content is touched, and on TBDR the load and store of the whole depth texture are saved (measured on a real device: a single-draw fullscreen pass takes half the GPU, with an extra D32S8 load and store attached).
// The scan follows real replay order across sub-list returns and across the remaining handles of this submission; end of submission = the pass must end. Undecidable cases such as a mid-pass clear or too-deep nesting always count as 'used'.
static const uint8_t *g_exec_next;   // The op following the current draw in the replay
static const uint8_t *g_ret[16]; static int g_ret_n;   // Call return stack of the replay (at OP_CALL the caller's next op is pushed)
static const uint64_t *g_rest; static int g_nrest;      // Handles in this submission still to be replayed after the current handle
void mtl_exec_rest(const uint64_t *h, int n) { g_rest = h; g_nrest = n; }
static uint64_t g_stat_ds_omit;      // Number of passes that attach less depth-stencil because the lookahead proved it unused (including stencil-only / depth-only)
static uint64_t g_stat_s_only, g_stat_d_only;   // Of those, the number of passes that attach only stencil / only depth
// Clear-only pass hoist (hoist_try): ops that were already executed early and must be skipped when replay reaches their original position (each group's SetRenderTargets and the clears in it).
// The exec loop, the store lookahead (sdx index) and the depth-stencil lookahead (ds_scan) all skip by it - otherwise the lookahead would take the clear at the original position as 'cleared again' and wrongly judge the early-cleared content redundant
#define HOIST_MAX 40   // At most 4 groups × (1 set-target op + 8 clears)
static const uint8_t *g_hoist_skip[HOIST_MAX]; static int g_hoist_n; static uint64_t g_hoist_groups;
static int hoist_skipped(const uint8_t *p) { for (int i = 0; i < g_hoist_n; i++) if (g_hoist_skip[i] == p) return 1; return 0; }
typedef struct { const uint8_t *p, *ret[8]; int ret_n, rest_i, bad; } fz_cursor;
static int fz_next(fz_cursor *c, const uint8_t **out, op_hdr *oh, const void **oa) {
    for (int guard = 0; guard < 1000000; guard++) {
        if (!c->p) return 0;
        op_hdr h; memcpy(&h, c->p, sizeof h); const void *a = c->p + 8;
        switch (h.op) {
            case OP_END:
                if (c->ret_n) c->p = c->ret[--c->ret_n];
                else if (c->rest_i < g_nrest) c->p = (const uint8_t *)g_rest[c->rest_i++];
                else { c->p = NULL; return 0; }
                continue;
            case OP_JUMP: memcpy(&c->p, a, 8); continue;
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); if (c->ret_n >= 8) { c->bad = 1; return 0; }
                c->ret[c->ret_n++] = c->p + h.size; c->p = (const uint8_t *)t; continue; }
            default: break;
        }
        const uint8_t *p = c->p; c->p += h.size;
        if (g_hoist_n && hoist_skipped(p)) continue;
        if (out) *out = p; if (oh) *oh = h; if (oa) *oa = a; return 1;
    }
    c->bad = 1; return 0;
}
// State of the readable-tile pass merge (merge B step 2): the earlier pass (phase 1) and the later pass (phase 2) share one render pass, and the attachments are the union
typedef struct {
    int active, phase, n1, nskip;
    int nreuse;
    const uint8_t *rt2_op, *reuse[8], *skip[12];
    p_set_rt rt1, rt2;                 // Logical targets of each of the two passes (R.rt is the union during the merge)
    int8_t p2map[8];                   // Logical color number of the later pass → physical attachment
    uint8_t lmap1[8], lmap2[8];        // Physical attachment → logical color number of this phase's draws (0xff = not written)
    uint8_t clr2[8], cd2f, cs2f; MTLClearColor cc2[8]; double cd2; uint32_t cs2;   // Clears at the start of the later pass (used as-is when the in-run dual render redraws)
    uint8_t wrote[8];                   // Physical attachments written by the later pass
    uint8_t cov2[8];                     // Physical attachments covered by the first draw of the later pass
} fuse_t;
static fuse_t g_fz; static uint64_t g_fz_n, g_fz_draws, g_fz_fallback;
typedef struct { int idx; p_draw dw; p_draw_idx di; p_scissor sc; int has_sc; void *S_; uint8_t pm[8]; } defer_snap;   // pm: group P target index → logical color number of this draw (identity for its own draws; a composed mapping for absorbed upstream groups)
typedef struct {
    int active, at_q, in_p, n, reason, reason_op, logged, reset_between; uint64_t born;   // born: creation serial (replay is ordered by it, not by slot)
    p_set_rt p_rt, q_rt;
    const uint8_t *p_end_op, *q_op;
    uint8_t clear_c[8], lmap[8];
    MTLClearColor cc[8];
    uint8_t clear_d, clear_s;
    double cd; uint32_t cs;
    uint64_t p_reads[512]; int nreads, p_dw, p_st;
    defer_snap snap[64];
} defer_t;
static defer_t g_defer_slots[4];
static defer_t *g_defer_cur = &g_defer_slots[0];
#define g_defer (*g_defer_cur)
static int sub_eq(const p_set_rt *a, int ka, const p_set_rt *b, int kb);
static uint64_t g_defer_n, g_defer_draws, g_defer_fallbacks;
static const uint8_t *g_defer_pend;   // The P-range end op of the group that is in its P range (NULL if none): the exec loop compares each op against just this one pointer
static int g_defer_nact, g_defer_natq;   // Number of active groups, number of groups that reached Q and await replay (the hot path only looks at the counts, no per-slot scan)
static int g_defer_ab = -1, g_defer_ab_on;
static void draw(const p_draw *dw, const p_draw_idx *di);
static int defer_has_in_p(void) { return g_defer_pend != NULL; }
static int defer_has_active(void) { return g_defer_nact > 0; }
static int defer_slot_free(void) { for (int i = 0; i < 4; i++) if (!g_defer_slots[i].active) return i; return -1; }
static int defer_same_q_depth(const defer_t *a, const p_set_rt *q) {
    if (!a->q_rt.depth || !q->depth) return 1;
    return sub_eq(&a->q_rt, 8, q, 8);
}
static int defer_next(int at_q_only, uint64_t after) {   // Slot number of the next active group (smallest creation serial that is > after); returns −1 if none
    int best = -1; for (int i = 0; i < 4; i++) { const defer_t *d = &g_defer_slots[i]; if (!d->active || (at_q_only && !d->at_q) || d->born <= after) continue; if (best < 0 || d->born < g_defer_slots[best].born) best = i; }
    return best;
}
static void defer_release_one(defer_t *d) { if (d->active) { g_defer_nact--; if (d->at_q) g_defer_natq--; if (d->in_p) g_defer_pend = NULL; } for (int i = 0; i < d->n; i++) free(d->snap[i].S_); memset(d, 0, sizeof *d); }
static void defer_clear_all(void) { for (int i = 0; i < 4; i++) defer_release_one(&g_defer_slots[i]); g_defer_nact = g_defer_natq = 0; g_defer_pend = NULL; }
static int hoist_take(const uint8_t *p) { for (int i = 0; i < g_hoist_n; i++) if (g_hoist_skip[i] == p) { g_hoist_skip[i] = g_hoist_skip[--g_hoist_n]; return 1; } return 0; }
enum { DS_FREE = 0, DS_USED = 1, DS_RET = 2 };
static int g_ds_why; static int g_ds_scanned; static int g_ds_need;   // g_ds_need: need accumulated during the scan, 1 depth 2 stencil   // Diagnostics: the opcode that triggered the 'used' verdict (for a draw, records 1000 + dt*100 + dw*10 + st), and the number of ops scanned
static int ds_scan(const uint8_t *p, ds_st *ds, int depth) {
    for (int guard = 0; guard < 1000000; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const void *a = p + 8; g_ds_scanned++;
        if (g_hoist_n && hoist_skipped(p)) { p += h.size; continue; }   // Clear pass already executed early
        switch (h.op) {
            case OP_END: return DS_RET;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); if (depth >= 8) return DS_USED;
                int r = ds_scan((const uint8_t *)t, ds, depth + 1); if (r != DS_RET) return r; break; }
            case OP_SET_RT: case OP_COPY_B2B: case OP_CLEAR_BUF: case OP_UPDATE_UB: case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_T2T: case OP_CLEAR_TEX: case OP_DISPATCH: return DS_FREE;
            case OP_CLEAR_COLOR: case OP_CLEAR_DS: g_ds_why = h.op; return DS_USED;
            case OP_BIND_STATE: { const p_bind_state *b = a; if (b->kind == 5) memcpy(ds, b->bytes, sizeof *ds); break; }
            case OP_DRAW: case OP_DRAW_IDX:   // Depth test / write → depth needed; stencil test → stencil needed; can stop once both are needed
                g_ds_need |= (ds->dt || ds->dw ? 1 : 0) | (ds->st ? 2 : 0);
                if (g_ds_need == 3) { g_ds_why = 1000 + ds->dt * 100 + ds->dw * 10 + ds->st; return DS_USED; } break;
            default: break;
        }
        p += h.size;
    }
    return DS_USED;
}
// Store lookahead decision (command by command, in real replay order):
//   Redundant: after being set as an attachment again and before any draw, it is cleared in full (color: all four channels of that index written; depth: depth clear; stencil: all 8 stencil bits cleared. Depth and stencil are decided separately and their write-back is set separately)
//   Needed: at a draw / dispatch it is bound to a texture or image slot of any stage (including ones already bound when the scan starts); used as copy source / copy destination / texture-clear target; drawn before cleared after becoming an attachment again (load)
// Different levels / layers of the same texture are decided separately (a downsample chain writes level k and reads level k−1); a sampler binding counts as a read of the whole texture (conservative)
static const uint8_t *g_exec_cur;   // The op currently being replayed (inclusive)
static void sd_need(sd_att *t, int why) { if (!t->state) { t->state = 1; t->why = (uint8_t)why; } }   // why: 1 a draw reads it as a texture 2 as an attachment, drawn before cleared 3 copy 4 texture clear 5 scanned to the end undecided 6 no start point / index unavailable
static int sdx_decide(uint8_t st[9]);   // Store lookahead decision (for the index see mtl_sub_index)
static void sd_finish(void) {
    if (!g_nsd) return;
    if (!tbdr_on()) {   // All optimizations off: always write back
        for (int i = 0; i < g_nsd; i++) { sd_att *t = &g_sd[i];
            if (t->depth == 1) [g_enc setDepthStoreAction:MTLStoreActionStore]; else if (t->depth == 2) [g_enc setStencilStoreAction:MTLStoreActionStore]; else [g_enc setColorStoreAction:MTLStoreActionStore atIndex:(NSUInteger)t->idx]; }
        g_nsd = 0; return; }
    uint8_t nst[9];
    if (sdx_decide(nst)) { for (int i = 0; i < g_nsd; i++) if (nst[i] == 2) g_sd[i].state = 2; else sd_need(&g_sd[i], nst[i] >> 2); }
    else for (int i = 0; i < g_nsd; i++) sd_need(&g_sd[i], 6);   // Pass ended outside the list / index unavailable: always write back
    for (int i = 0; i < g_nsd; i++) {
        sd_att *t = &g_sd[i]; int udc_dead = udc_on() && t->load && !t->wrote;
        // A color attachment written in the same pass as the window texture does not enter the ledger: it is a link in the present chain, not a transient attachment that is 'reset every frame before use'.
        // The in-run 'upscale' pass writes two textures at once - scene texture + window texture - and the UI composite was merged into the same pass, so in-run nobody reads back the scene texture; as soon as the game stops drawing 3D (loading screen)
        // a separate composite pass samples it afterwards instead. 'About to enter the loading screen' is only known after the previous frame was submitted, so prediction cannot stop it: on iPad true fullscreen every install hits it once on the first act change
        // (the 2732×2048 attachment of frame N was not written back and is sampled for 4 consecutive frames starting at frame N+1; the key is permanently disabled). At the iPhone aspect ratio the same-size key already sees a reader during the trial period and is silently disabled.
        int xv_win = 0; for (int k = 0; k < R.rt.n && k < 8; k++) if (R.rt.color[k] && xv_present(R.rt.color[k]->serial)) xv_win = 1;
        uint64_t xtag = 0; int xvcand = xv_on() && !xv_win && t->depth == 0 && t->state != 2 && t->why == 5 && xv_first_good(t->ser, t->lv, t->sl) && !xv_present(t->ser)
                                        && !xv_disabled(xtag = xv_tag_pass(R.rt.color[t->idx], t->lv, t->idx, &R.rt));
        int xvdead = xvcand && xv_trial_get(xtag, 0) >= XV_TRIAL;   // Only after the trial period is the write-back really skipped
        if (xvcand && !xvdead) xv_set(t->ser, t->lv, t->sl, XV_PROBE, xtag);   // Trial period: write back as usual, only track the next touch
        int dead = t->state == 2 || udc_dead || xvdead; MTLStoreAction sa = dead ? MTLStoreActionDontCare : MTLStoreActionStore;
        if (xvdead) { xv_set(t->ser, t->lv, t->sl, XV_INVALID, xtag); g_xv_dc++; }
        if (t->depth == 1) [g_enc setDepthStoreAction:sa]; else if (t->depth == 2) [g_enc setStencilStoreAction:sa]; else [g_enc setColorStoreAction:sa atIndex:(NSUInteger)t->idx];
        g_sd_n_all++; if (dead) { g_sd_n_dead++; g_sd_dead_bytes += t->bytes; g_bw_store -= t->bytes; if (udc_dead) g_udc_saved_bytes += t->bytes; }
        if (!t->state) t->why = 5;
        PD("Z %llu %s ser=%llu lv%u sl%u %s why=%d by=%s slot=%d", g_stat_pass, t->depth == 1 ? "d" : t->depth == 2 ? "s" : "c", (unsigned long long)t->ser, t->lv, t->sl, xvdead ? "dontcare-xv" : dead ? "dontcare" : "store", dead ? 0 : t->why, t->why == 1 && t->wkey ? t->wkey : "-", t->wslot);
        if (udc_on()) PD("U %llu %s ser=%llu lv%u sl%u runtime=%s reason=%s state=%d load=%d", g_stat_pass, t->depth == 1 ? "d" : t->depth == 2 ? "s" : "c", (unsigned long long)t->ser, t->lv, t->sl, t->wrote ? "写过" : "没写", t->wwhy == 1 ? "图像写" : t->wwhy == 2 ? "颜色写掩码" : t->wwhy == 3 ? "片元输出" : t->wwhy == 4 ? "深度写" : t->wwhy == 5 ? "模板写" : "无", t->state, t->load);
    }
    g_nsd = 0;
}
// Continue scanning in real replay order: current list -> callers at each level (return stack from the top down) -> remaining handles of this submit; the render pass necessarily ends when the submit ends (rp_flush before commit)
// Coverage proof (holds only if all are satisfied; anything unrecognized is treated as not holding):
//   Shader: the vertex position xyz is taken as-is from one vertex attribute, w is 1 or the same attribute's .w (dmsh_build raster_facts); the fragment does not discard, does not write sample_mask, and writes this color index
//   State: this attachment has no blending, a full write mask, no alpha-to-coverage, fill mode, no depth test (or ALWAYS), no stencil test, single sample, not 3D
//   Vertices: non-indexed or 16 / 32-bit indexed draws, with indices and positions both read from CPU-visible pool memory; the scissor / viewport / geometry / winding / depth clipping decisions are in nvn_cover.h (pure functions, with unit tests)
// Read out attribute a of all vertices of this draw (non-indexed or 16 / 32-bit indexed; vertex number = first + k or index value + base_vertex), with missing components filled with 0 / 1 per the fetch rules.
// Returns 0 on success, a reason code on failure (6 vertex count / index type, 7 attribute index, 8 format / stream, 9 out of bounds, 10 address)
static int attr_read(int a, const p_draw *dw, const p_draw_idx *di, float P[64][4]) {
    int n = dw ? dw->count : di->count;
    if (n < 1 || n > 64 || (dw && dw->first < 0) || (di && (di->itype < 1 || di->itype > 2))) return 6;
    if (a < 0 || a >= S.nattr || S.attr[a].stream >= 16) return 7;
    va_st at = S.attr[a]; vs_st sm = S.stream[at.stream]; NVNbufferRange vb = S.vbuf[at.stream];
    MTLVertexFormat vf = vfmt(at.fmt); int nc = vf == MTLVertexFormatFloat2 ? 2 : vf == MTLVertexFormatFloat3 ? 3 : vf == MTLVertexFormatFloat4 ? 4 : 0;
    if (!nc || !sm.stride || sm.divisor || !vb.address) return 8;
    const uint8_t *ib = NULL; int isz = di ? (di->itype == 1 ? 2 : 4) : 0;
    if (di && pend_hits(di->ibuf, (uint64_t)n * isz)) return 10;   // indices written by a copy / update earlier in this submission: the CPU still sees the old ones
    if (di) { void *m; uint64_t o; if (!mtl_addr(di->ibuf, (uint64_t)n * isz, &m, &o) || !(ib = [(__bridge id<MTLBuffer>)m contents])) return 10; ib += o; }
    for (int k = 0; k < n; k++) {
        int64_t vid = dw ? (int64_t)dw->first + k : (isz == 2 ? (int64_t)((const uint16_t *)ib)[k] : (int64_t)((const uint32_t *)ib)[k]) + di->base_vertex;
        uint64_t off = (uint64_t)vid * sm.stride + at.offset;
        if (vid < 0 || off + 4 * nc > vb.size) return 9;
        if (pend_hits(vb.address + off, 4 * nc)) return 10;   // position written on the GPU earlier in this submission
        void *mb; uint64_t mo; const uint8_t *base;
        if (!mtl_addr(vb.address + off, 4 * nc, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) return 10;
        float v[4] = {0, 0, 0, 1}; memcpy(v, base + mo, 4 * nc); memcpy(P[k], v, sizeof v);
    }
    return 0;
}
static int first_draw_covers(int i, id<MTLTexture> t, NSUInteger level) {
    const p_draw *dw = g_fd_dw; const p_draw_idx *di = g_fd_di;
    int n = dw ? dw->count : di->count, prim = dw ? dw->prim : di->prim;
    __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil; if (!pr) return g_fd_why = 1, 0;
    NvShader *vs = pr->sh[0], *fs = pr->sh[1];
    if (!vs || !fs || vs->rf.pos_attr < 0 || fs->rf.may_discard || fs->rf.writes_smask || !(fs->rf.color_mask >> i & 1)) return g_fd_why = 2, 0;
    if (!t || t.sampleCount != 1 || t.textureType == MTLTextureType3D) return g_fd_why = 3, 0;
    if ((S.color.blend_en >> i & 1) || ((S.chmask.mask >> (4 * i)) & 0xf) != 0xf || S.ms.a2c || S.poly.mode != NVN_POLYGON_MODE_FILL) return g_fd_why = 4, 0;
    if (R.rt.depth && ((S.ds.dt && S.ds.df != NVN_DEPTH_FUNC_ALWAYS) || S.ds.st)) return g_fd_why = 5, 0;
    if (n < 3 || n > 64) return g_fd_why = 6, 0;
    float P[64][4]; int why = attr_read(vs->rf.pos_attr, dw, di, P);
    if (why) return g_fd_why = why, 0;
    if (vs->rf.pos_w1) for (int k = 0; k < n; k++) P[k][3] = 1.0f;   // Position w is the constant 1 (the attribute's .w is not taken)
    int sc[4] = {R.sc.x, R.sc.y, R.sc.w, R.sc.h}; float vp[4] = {S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3]};
    g_fd_why = 11;
    return nvn_cover((const float (*)[4])P, n, prim, (int)S.poly.cull, S.poly.front == NVN_FRONT_FACE_CCW,
                     S.has_vp, vp, R.has_sc, sc, (long)MAX(t.width >> level, 1), (long)MAX(t.height >> level, 1));
}

static int state_op(uint32_t op, const void *a) {
    p_bind_handles one;
    if (op == OP_BIND_TEX1) { a = bind_rec(op, a, &one); op = OP_BIND_TEX; }
    switch (op) {
        case OP_SCISSOR: R.sc = *(const p_scissor *)a; R.has_sc = 1; break;
        case OP_BIND_PROGRAM: { const p_bind_program *bp_ = a; const prog_t *np = bind_stages(bp_->prog, bp_->stages); if (np != S.prog) { S.prog = np; g_pso_dirty = 1; args_dirty_all(); } break; }
        case OP_BIND_STATE: {
            const p_bind_state *b = a; void *dst = NULL; size_t n = 0;
            blend_st bs;
            switch (b->kind) {
                case 0: memcpy(&bs, b->bytes, sizeof bs); dst = &S.blend[bs.target]; n = sizeof bs; break;
                case 1: dst = &S.color; n = sizeof S.color; break;
                case 2: dst = &S.chmask; n = sizeof S.chmask; break;
                case 3: dst = &S.ms; n = sizeof S.ms; break;
                case 4: dst = &S.poly; n = sizeof S.poly; break;
                case 5: dst = &S.ds; n = sizeof S.ds; break;
            }
            if (dst && memcmp(dst, b->bytes, n)) { memcpy(dst, b->bytes, n); if (b->kind == 5) g_dss_dirty = 1; else g_pso_dirty = 1; }
            break;
        }
        case OP_BIND_VATTR: { const p_bind_vattr *b = a; if (S.nattr != b->n || memcmp(S.attr, b->st, 4 * b->n)) { memcpy(S.attr, b->st, 4 * b->n); S.nattr = b->n; g_pso_dirty = 1; } break; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; if (S.nstream != b->n || memcmp(S.stream, b->st, 8 * b->n)) { memcpy(S.stream, b->st, 8 * b->n); S.nstream = b->n; g_pso_dirty = 1; } break; }
        case OP_BIND_VBUF: { const p_bind_vbuf *b = a; memcpy(&S.vbuf[b->first], b->r, sizeof *b->r * b->n); break; }
        case OP_BIND_UBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index < 16 && memcmp(&S.ubo[b->stage][b->index], b, sizeof *b)) { S.ubo[b->stage][b->index] = *b; g_args_dirty[b->stage] = 1; } break; }
        case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index < 16 && memcmp(&S.ssbo[b->stage][b->index], b, sizeof *b)) { S.ssbo[b->stage][b->index] = *b; g_args_dirty[b->stage] = 1; } break; }
        case OP_BIND_TEX: { const p_bind_handles *b = a; bh_fresh(b); if (b->stage < 6 && b->first + b->n <= NTEX && memcmp(&S.tex[b->stage][b->first], b->h, 8 * b->n)) { memcpy(&S.tex[b->stage][b->first], b->h, 8 * b->n); g_args_dirty[b->stage] = 1; if (b->first + b->n > NVN_SEPTEX_SLOT0) { static int once; if (!once++) MLOG("unified NVN texture ABI: accepted separate texture slots %d..%d for stage %d", b->first, b->first + b->n - 1, b->stage); } } break; }
        case OP_BIND_SAMP: { const p_bind_handles *b = a; bh_fresh(b); if (b->stage < 6 && b->first + b->n <= 128 && memcmp(&S.samp[b->stage][b->first], b->h, 8 * b->n)) { memcpy(&S.samp[b->stage][b->first], b->h, 8 * b->n); g_args_dirty[b->stage] = 1; } break; }
        case OP_BIND_IMG: { const p_bind_handles *b = a; bh_fresh(b); if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 32) { if (memcmp(&S.img[b->stage][b->first], b->h, 8 * b->n)) { memcpy(&S.img[b->stage][b->first], b->h, 8 * b->n); g_args_dirty[b->stage] = 1; } } else RED_ONCE(31, "图像槽越界 阶段 %d 起 %d 数 %d", b->stage, b->first, b->n); break; }
        case OP_STENCIL: {
            const p_stencil *b = a;
            if (b->which != 2) g_dss_dirty = 1;
            for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) S.st_vmask[f] = b->value_mask; else if (b->which == 1) S.st_wmask[f] = b->mask; else S.st_ref[f] = b->ref; }
            break;
        }
        case OP_POLY_OFFSET: S.po = *(const p_poly_offset *)a; break;
        case OP_VIEWPORTS: { const p_viewports *b = a; for (int i = 0; i < b->n; i++) memcpy(S.vp[b->first + i], b->v[i], 16); S.has_vp = 1; break; }
        case OP_DEPTH_RANGE: { const p_depth_range *b = a; S.dr[0] = b->n; S.dr[1] = b->f; break; }
        case OP_DEPTH_CLAMP: S.depth_clamp = ((const p_depth_clamp *)a)->on; break;
        case OP_BLEND_COLOR: memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
        case OP_COPY_STRIDE: { const p_copy_stride *cs = a; if (cs->image) S.copy_img_stride = cs->stride; else S.copy_stride = cs->stride; break; }
        case OP_TF_BIND: { const p_tf_bind *b = a; if (b->index >= 0 && b->index < 4) S.tf[b->index] = *b; break; }
        case OP_TF_STATE: { const p_tf_state *b = a; if (b->what == 0) { S.tf_on = 1; memset(S.tf_off, 0, sizeof S.tf_off); } else S.tf_on = b->what == 3; break; }
        case OP_RAST_DISCARD: S.rast_discard = ((const p_rast_discard *)a)->on; break;
        default: return 0;
    }
    return 1;
}

static void rt_sub(const p_set_rt *r, int k, uint64_t *ser, int *lv, int *sl);

// Prediction for parallel precompilation (see the note before pso_get): scan from after the current operation to the end of this submit, replaying only state; at a draw, compute the pipeline key from "the state at that time + that render pass's attachment formats" and dispatch those not yet built to the background.
// Whether a render pass actually attaches depth / stencil is only decided at replay (depth-only / stencil-only / neither attached are omitted), so one key is computed for each possible attach mode -- guessing too many only means wasted background compiles and does not affect the result.
static void pso_predict_rest(void) {
    if (!g_exec_next) return;
    static typeof(S) Ssave; memcpy(&Ssave, &S, sizeof S);
    int pso_dirty = g_pso_dirty, dss_dirty = g_dss_dirty; uint8_t topology = g_pso_topology;
    uint32_t cf[8]; memcpy(cf, g_pass.cfmt, sizeof cf); uint32_t df = g_pass.dfmt, sf = g_pass.sfmt, samples = g_pass.samples, dfull = df, sfull = sf; int kicked0 = (int)g_pp_kicked, draws = 0;
    fz_cursor cur = {.p = g_exec_next};
    // Collect them all first, then dispatch: keys for "attachments attached as-is" go first (most likely to be used), the other attach modes after -- background concurrency is limited, and what is dispatched first is compiled first
    enum { QN = 160 }; static struct { pso_key k; void *vs, *fs; } qa[QN], qb[QN]; int na = 0, nb = 0;
    for (int guard = 0; guard < 200000 && na < QN; guard++) {
        const uint8_t *q; op_hdr hq; const void *aq;
        if (!fz_next(&cur, &q, &hq, &aq)) break;
        if (hq.op == OP_SET_RT) {
            const p_set_rt *r = aq; memset(cf, 0, sizeof cf); dfull = sfull = 0; samples = 1;
            for (int i = 0; i < r->n && i < 8; i++) { id<MTLTexture> t = r->color[i] && !rt_dup_slot(r, i) ? tex_mtl(r->color[i]) : nil; if (t && nvn_fmt_bc(r->color[i]->d.format)) t = nil; if (t) { cf[i] = (uint32_t)rt_view_fmt(t, &r->cview[i], r->has_cview[i]); samples = (uint32_t)t.sampleCount; } }   // BC targets are never attached (see attach)
            id<MTLTexture> dt = r->depth ? tex_mtl(r->depth) : nil;
            if (dt) { MTLPixelFormat f = dt.pixelFormat; samples = (uint32_t)dt.sampleCount;
                dfull = f != MTLPixelFormatStencil8 ? (uint32_t)f : 0; sfull = (f == MTLPixelFormatStencil8 || f == MTLPixelFormatDepth32Float_Stencil8) ? (uint32_t)f : 0; }
            df = dfull; sf = sfull; continue;
        }
        if (hq.op == OP_DRAW || hq.op == OP_DRAW_IDX) {
            if (S.rast_discard) continue;
            draws++;
            g_pso_topology = pso_topology_class(hq.op == OP_DRAW ? ((const p_draw *)aq)->prim : ((const p_draw_idx *)aq)->prim);
            __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil; if (!pr || !pr->sh[0]) continue;
            __unsafe_unretained NvShader *vs = pr->sh[0], *fs = pr->sh[1];
            const uint32_t dv[4] = {dfull, 0, 0, dfull}, sv[4] = {sfull, 0, sfull, 0};
            for (int v = 0; v < 4; v++) {
                if (v && dv[v] == dv[0] && sv[v] == sv[0]) continue;
                if (v == 2 && !sfull) continue; if (v == 3 && (!dfull || !sfull)) continue;
                pso_key k; pso_key_fill(&k, vs, fs, cf, dv[v], sv[v], samples, 0);
                if (hm_get_id(&g_pso, &k)) continue;
                int dup = 0; for (int i = 0; i < (v ? nb : na) && !dup; i++) dup = !memcmp(&(v ? qb : qa)[i].k, &k, sizeof k);
                if (dup) continue;
                if (!v && na < QN) { qa[na].k = k; qa[na].vs = (__bridge void *)vs; qa[na].fs = (__bridge void *)fs; na++; }
                else if (v && nb < QN) { qb[nb].k = k; qb[nb].vs = (__bridge void *)vs; qb[nb].fs = (__bridge void *)fs; nb++; }
            }
            continue;
        }
        if (hq.op == OP_CLEAR_COLOR || hq.op == OP_CLEAR_DS) continue;
        state_op(hq.op, aq);
    }
    (void)df; (void)sf;
    for (int i = 0; i < na; i++) pso_pend_kick(&qa[i].k, (__bridge NvShader *)qa[i].vs, (__bridge NvShader *)qa[i].fs);
    for (int i = 0; i < nb; i++) pso_pend_kick(&qb[i].k, (__bridge NvShader *)qb[i].vs, (__bridge NvShader *)qb[i].fs);
    memcpy(&S, &Ssave, sizeof S); args_dirty_all(); g_pso_dirty = pso_dirty; g_dss_dirty = dss_dirty; g_pso_topology = topology;
    static int logged; if (g_pp_kicked > (uint64_t)kicked0 && logged++ < 20) MLOG("并行预编：本次提交余下 %d 次绘制，丢给后台 %d 条管线", draws, (int)g_pp_kicked - kicked0);
}

static void fz_probe_cov2(const p_set_rt *r2, const uint8_t *first, const int8_t *p2map, const uint8_t *clr, int n1, int n) {
    static typeof(S) Ssave; memcpy(&Ssave, &S, sizeof S); typeof(R) Rsave = R;
    const p_draw *fdw = g_fd_dw; const p_draw_idx *fdi = g_fd_di; int fdwhy = g_fd_why;
    int pso_dirty = g_pso_dirty, dss_dirty = g_dss_dirty;
    op_hdr fh; memcpy(&fh, first, sizeof fh);
    g_fd_dw = fh.op == OP_DRAW ? (const p_draw *)(first + 8) : NULL;
    g_fd_di = fh.op == OP_DRAW_IDX ? (const p_draw_idx *)(first + 8) : NULL;
    R.rt = *r2; R.has_rt = 1;
    fz_cursor cur = {.p = g_exec_next};
    for (;;) {
        const uint8_t *q; op_hdr hq; const void *aq;
        if (!fz_next(&cur, &q, &hq, &aq)) break;
        if (q == first) break;
        if (hq.op == OP_DRAW || hq.op == OP_DRAW_IDX || hq.op == OP_SET_RT || hq.op == OP_CLEAR_COLOR || hq.op == OP_CLEAR_DS) continue;
        state_op(hq.op, aq);
    }
    for (int j = n1; j < n && j < 8; j++) {
        int k = -1;
        for (int q = 0; q < 8; q++) if (p2map[q] == j) { k = q; break; }
        if (k < 0 || clr[k] || !r2->color[k]) continue;
        uint64_t ser; int level, slice;
        rt_sub(r2, k, &ser, &level, &slice);
        (void)ser; (void)slice;
        g_fz.cov2[j] = (uint8_t)first_draw_covers(k, tex_mtl(r2->color[k]), (NSUInteger)level);
    }
    memcpy(&S, &Ssave, sizeof S); args_dirty_all(); R = Rsave;
    g_fd_dw = fdw; g_fd_di = fdi; g_fd_why = fdwhy;
    g_pso_dirty = pso_dirty; g_dss_dirty = dss_dirty;
}

static int fz_load_covers(int i, id<MTLTexture> t, NSUInteger level) {
    if (g_fz.active && g_fz.phase == 1 && i >= g_fz.n1) return g_fz.cov2[i];
    return first_draw_covers(i, t, level);
}

// Probe wrap-up: for every attachment changed to clear to the sentinel color, read back that level / layer, compare pixel by pixel with a same-format 1x1 sentinel pixel, and count what remains (> 0 means the coverage proof is wrong, report a failure)
static void lp_finish(void) {
    id<MTLCommandBuffer> dcb = q_cb();
    NSArray *ps = g_lp; g_lp = nil;
    for (NSArray *e in ps) {
        id<MTLTexture> t = e[0]; NSUInteger lv = [e[1] unsignedIntegerValue], sl = [e[2] unsignedIntegerValue];
        NSUInteger W = MAX(t.width >> lv, 1), H = MAX(t.height >> lv, 1), bpp = (NSUInteger)(att_bytes(t, lv) / (W * H));
        MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:1 height:1 mipmapped:NO];
        rd.usage = MTLTextureUsageRenderTarget; rd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> ref = [g_dev newTextureWithDescriptor:rd];
        MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
        pd.colorAttachments[0].texture = ref; pd.colorAttachments[0].loadAction = MTLLoadActionClear; pd.colorAttachments[0].storeAction = MTLStoreActionStore;
        pd.colorAttachments[0].clearColor = [e[6] intValue] == 3 ? MTLClearColorMake(NAN, NAN, NAN, NAN) : [e[6] intValue] == 2 ? MTLClearColorMake(0.777, 0.222, 0.555, 0.888) : MTLClearColorMake(0.123, 0.456, 0.789, 0.321);
        [[dcb renderCommandEncoderWithDescriptor:pd] endEncoding];
        id<MTLBuffer> bt = [g_dev newBufferWithLength:W * H * bpp options:MTLResourceStorageModeShared], br = [g_dev newBufferWithLength:bpp options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> b = [dcb blitCommandEncoder];
        [b copyFromTexture:t sourceSlice:sl sourceLevel:lv sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1) toBuffer:bt destinationOffset:0 destinationBytesPerRow:W * bpp destinationBytesPerImage:W * H * bpp];
        [b copyFromTexture:ref sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(1, 1, 1) toBuffer:br destinationOffset:0 destinationBytesPerRow:bpp destinationBytesPerImage:bpp];
    enc_b_end(b);
        unsigned long long pass = [e[3] unsignedLongLongValue], ser = [e[5] unsignedLongLongValue]; int idx = [e[4] intValue];
        [dcb addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c;
            const uint8_t *p = bt.contents, *r = br.contents; uint64_t left = 0; char at[96] = ""; size_t ao = 0;
            for (NSUInteger k = 0; k < W * H; k++) if (!memcmp(p + k * bpp, r, bpp)) { if (left++ < 4) ao += snprintf(at + ao, sizeof at - ao, " (%lu,%lu)", (unsigned long)(k % W), (unsigned long)(k / W)); }
            if (left) MLOG("【红】读入可省探针：通道 %llu c%d ser=%llu lv%lu sl%lu %lux%lu 剩哨兵像素 %llu，前几个位置%s", pass, idx, ser, (unsigned long)lv, (unsigned long)sl, (unsigned long)W, (unsigned long)H, (unsigned long long)left, at);
            else MLOG("读入可省探针：通道 %llu c%d ser=%llu lv%lu sl%lu %lux%lu 全覆盖（哨兵剩 0）", pass, idx, ser, (unsigned long)lv, (unsigned long)sl, (unsigned long)W, (unsigned long)H);
        }];
    }
    [dcb commit];
}
static int rt_same(const p_set_rt *a, const p_set_rt *b) {   // Same set of targets: compare by texture serial and view (not by object pointer: the game may copy texture objects by value)
    if (a->n != b->n || !a->depth != !b->depth || (a->depth && a->depth->serial != b->depth->serial) || a->has_dview != b->has_dview) return 0;
    if (a->has_dview && memcmp(&a->dview, &b->dview, sizeof a->dview)) return 0;
    for (int k = 0; k < a->n && k < 8; k++) {
        if (!a->color[k] != !b->color[k] || (a->color[k] && a->color[k]->serial != b->color[k]->serial) || a->has_cview[k] != b->has_cview[k]) return 0;
        if (a->has_cview[k] && memcmp(&a->cview[k], &b->cview[k], sizeof a->cview[k])) return 0;
    }
    return 1;
}
// Experimental RT range candidate: only ignore positive nlevels differences not used by the actual render attachments; other fields stay word-for-word identical.
#define NVN_RT_RANGE_IMPL 1
static int rt_range_shape(const p_set_rt *a, const p_set_rt *b) {
    if (a->n != 1 || b->n != 1 || !a->color[0] || !b->color[0] || !a->has_cview[0] || !b->has_cview[0] ||
        !a->depth != !b->depth || a->has_dview != b->has_dview ||
        (a->has_dview && memcmp(&a->dview,&b->dview,sizeof a->dview))) return 0;
    const tex_t *x=a->color[0], *y=b->color[0];
    if (!x->serial || x->serial != y->serial || x->d.target != NVN_TEXTURE_TARGET_2D || y->d.target != NVN_TEXTURE_TARGET_2D ||
        x->d.samples > 1 || y->d.samples > 1 || (x->has_layout && x->mem) || (y->has_layout && y->mem) ||
        memcmp(&x->d,&y->d,sizeof x->d) || x->mem != y->mem || x->pool != y->pool || x->off != y->off) return 0;
    const view_t *v=&a->cview[0], *w=&b->cview[0]; int levels=x->d.levels;
    if (!v->set_levels || !w->set_levels || v->base_level < 0 || v->base_level >= levels ||
        v->nlevels <= 0 || v->nlevels > levels-v->base_level || w->nlevels <= 0 || w->nlevels > levels-v->base_level ||
        (v->set_layers && (v->min_layer || v->nlayers != 1)) || (v->set_format && v->format != x->d.format) ||
        (v->set_target && v->target != NVN_TEXTURE_TARGET_2D)) return 0;
    view_t cv=*w; cv.nlevels=v->nlevels;
    if (memcmp(v,&cv,sizeof cv)) return 0;
    if (a->depth) { const tex_t *d=a->depth,*e=b->depth;
        if (!d->serial || d->serial != e->serial || d->d.target != NVN_TEXTURE_TARGET_2D || e->d.target != NVN_TEXTURE_TARGET_2D ||
            d->d.samples > 1 || e->d.samples > 1 || (d->has_layout && d->mem) || (e->has_layout && e->mem) ||
            memcmp(&d->d,&e->d,sizeof d->d) || d->mem != e->mem || d->pool != e->pool || d->off != e->off) return 0;
        int dl=a->has_dview && a->dview.set_levels ? a->dview.base_level : 0;
        if (dl < 0 || dl >= d->d.levels || mip_extent(d->d.width,dl) < mip_extent(x->d.width,v->base_level) ||
            mip_extent(d->d.height,dl) < mip_extent(x->d.height,v->base_level)) return 0; }

    return v->nlevels != w->nlevels;
}
#define NVN_RT_RANGE_DEPTH_IMPL 1
static void rt_range_note_depth(void) { if (rt_range_on() && S.ds.dw) g_pass.range_depth_nonro=1; }
static int rt_range_same(const p_set_rt *a, const p_set_rt *b) {
    if (!rt_range_on() || !g_enc || g_fz.active || g_pass.range_external || g_pass.range_depth_nonro || !S.ds.dt || S.ds.dw || S.ds.st || g_enc_mode != 4 || !g_pass.hd || g_pass.hs ||
        (g_vis_on && !g_enc_has_vis) || g_pass.reg_gen != g_reg_gen || g_pass.ser_gen != g_ser_gen || g_pass.range_dirty != nvn_dirty_now() || !rt_range_shape(a,b) || !b->depth) return 0;
    id<MTLTexture> color=tex_mtl(b->color[0]), depth=tex_mtl(b->depth);
    int lv=b->cview[0].base_level;
    return mip_extent(color.width,lv) == g_pass.tw && mip_extent(color.height,lv) == g_pass.th &&
        color == g_pass.range_color && depth == g_pass.range_depth && color.textureType == MTLTextureType2D &&
        depth.textureType == MTLTextureType2D && color.sampleCount == 1 && depth.sampleCount == 1 &&
        g_pass.reg_gen == g_reg_gen && g_pass.ser_gen == g_ser_gen && g_pass.range_dirty == nvn_dirty_now();
}
static int draw_samples_rt(NvProg *pr) {   // Sampled attachments keep the original conservative ser-level guard, not split by level/layer
    uint64_t rs[9]; int nr = 0;
    for (int k = 0; k < R.rt.n && k < 8; k++) if (R.rt.color[k]) rs[nr++] = R.rt.color[k]->serial;
    if (R.rt.depth) rs[nr++] = R.rt.depth->serial;
    if (!pr) return 1;
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 && sh->stage < 6 ? S.img[sh->stage][b->reg] : 0; else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = b->reg < NTEX && sh->stage < 6 ? S.tex[sh->stage][b->reg] : 0; else continue;
            int tid = (int)(h & 0xfffff); if (!tid) continue;
            pthread_mutex_lock(&g_rmu); uint64_t ser = tid < g_ntreg && g_treg[tid].ok ? g_treg[tid].t.serial : 0; pthread_mutex_unlock(&g_rmu);
            for (int j = 0; j < nr; j++) if (ser && ser == rs[j]) return 1; } }
    return 0;
}
// The whole render pass's need for depth / stencil (bits: 1 depth 2 stencil; 3 = both needed or cannot be proven). Starting from the draw that opens the render pass, scan in real replay order to the end of the render pass:
// the attached clears and this draw's state are counted first; later draws accumulate by their depth-stencil state; anything unprovable such as a mid-pass clear or nesting too deep is treated as both needed
static int pass_ds_need(void) {
    int need = (R.clear_d || S.ds.dt || S.ds.dw ? 1 : 0) | (R.clear_s || S.ds.st ? 2 : 0);
    if (!R.rt.depth || need == 3 || !g_exec_next || g_ret_n > 16) {
        PD("S %llu 不前瞻 depth=%d clear_d=%d clear_s=%d dt=%d dw=%d st=%d next=%d ret=%d", g_stat_pass + 1, R.rt.depth != NULL, R.clear_d, R.clear_s, (int)S.ds.dt, (int)S.ds.dw, (int)S.ds.st, g_exec_next != NULL, g_ret_n);
        return 3;
    }
    ds_st ds = S.ds; g_ds_why = -1; g_ds_scanned = 0; g_ds_need = need;
    int r = ds_scan(g_exec_next, &ds, 0);
    for (int k = g_ret_n - 1; r == DS_RET && k >= 0; k--) r = ds_scan(g_ret[k], &ds, 0);
    for (int k = 0; r == DS_RET && k < g_nrest; k++) r = ds_scan((const uint8_t *)g_rest[k], &ds, 0);
    int out = r == DS_USED ? 3 : g_ds_need;
    PD("S %llu 前瞻 结果=%d 需求=%d（1 深度 2 模板）原因=%d 扫过 %d 条 返回栈 %d 余句柄 %d", g_stat_pass + 1, r, out, g_ds_why, g_ds_scanned, g_ret_n, g_nrest);
    return out;
}
// Pure-clear render pass hoisting: when a render pass is opened, scan forward, and if the shape is "several draws of this render pass -> several groups (SetRenderTargets + only clears) -> back to the same targets as this render pass",
// then hoist those groups of clears to execute before this render pass and skip them at their original positions, and on returning to the same targets the same-target continuation picks up -- this render pass's attachments are no longer stored whole and loaded again for the clears sandwiched in between
// (character select page: grunge and particles both draw 28 / 27 level 0 + depth, with only levels 1 and 2 cleared in between).
// Equivalence conditions (hoist only if all hold, otherwise leave as-is): the scan range has only draws, state and these clears, no copies / dispatches / texture clears / sub-lists; the cleared subresources (texture + level + layer)
// are disjoint from this render pass's attachments; the cleared textures are neither on any current texture / image slot nor bound within the scan range (conservative: regardless of whether the program samples them); the scissor at the time of the clear covers the whole block (same requirement as executing at the original position)
static uint64_t h_ser(uint64_t h) { int tid = (int)(h & 0xfffff); return tid ? atomic_load_explicit(&g_tser[tid], memory_order_relaxed) : 0; }
// ---------- Store lookahead index: at the start of a submit do one linear scan in replay order and record events per texture; when a render pass ends, binary-search each attachment for "the first valid event afterwards" to decide ----------
// Replaces the original "scan the whole remainder forward at every render pass end" (pass count x command count, 49 render passes per frame in camp). Cross-checked per attachment against the original scan: after 0 mismatches over the 17 pages of the screenshot gate + a camp run with about 340,000 attachment decisions (about 46,000 redundant), the original scan was removed.
// Events (seq = replay sequence number): READ a draw / dispatch reads this texture per program reflection; ATT it is a current attachment at a draw; CLEAR cleared as a whole block while an attachment; COPY / CLRTEX either end of a copy / the cleared texture.
// Decision (the first valid event decides): READ / COPY / CLRTEX -> needed; ATT whose SetRenderTargets is after the render pass end -> needed (load); CLEAR with the same -> redundant; no event -> needed.
// On any uncertainty (replay sequence number does not match the index, nesting too deep) the index is invalidated and all remaining render passes of this submit store (storing is always correct, it just saves less bandwidth).
typedef struct { uint32_t seq, rtseq; uint64_t ser; const uint8_t *op; uint16_t lv, sl; uint8_t kind, plane, anyls, why; } sdx_ev;   // plane 0 color 8 depth-stencil (ATT) 1 depth 2 stencil (CLEAR)
enum { SDX_READ = 1, SDX_NEED, SDX_ATT, SDX_CLEAR };
#define SDX_NONE 0xffffffffu
static sdx_ev *g_sdx, *g_sdx_tmp; static uint32_t g_sdx_n, g_sdx_cap; static int g_sdx_ok;   // Index valid
// Candidate textures (those that have been attachments) -> buckets: open addressing; a texture that has not yet been an attachment when an event occurs is irrelevant to any render pass that ended before then, so it is not recorded
#define SDX_HB 2048
static uint64_t g_sdx_hk[SDX_HB]; static uint16_t g_sdx_hb[SDX_HB]; static uint32_t g_sdx_nb, g_sdx_bcnt[SDX_HB + 1], g_sdx_bst[SDX_HB + 1];
static int sdx_bucket(uint64_t ser, int add) {   // Returns the bucket number; returns -1 if not in the set (and not adding); invalidates the index when full
    if (!ser) return -1;
    for (uint32_t i = (uint32_t)(ser * 0x9E3779B97F4A7C15ull >> 53), n = 0; n < SDX_HB; i = (i + 1) & (SDX_HB - 1), n++) {
        if (g_sdx_hk[i] == ser) return g_sdx_hb[i];
        if (!g_sdx_hk[i]) { if (!add) return -1; if (g_sdx_nb >= SDX_HB / 2) { g_sdx_ok = 0; return -1; } g_sdx_hk[i] = ser; g_sdx_hb[i] = (uint16_t)g_sdx_nb; return (int)g_sdx_nb++; }
    }
    return -1;
}
static const uint8_t **g_sdx_op; static uint32_t g_sdx_nop, g_sdx_opcap;
static uint32_t g_exec_seq;   // Sequence number of the current operation during replay
static uint64_t g_pp_slot[6][NTEX + 32]; static const prog_t *g_pp_prog; static const p_set_rt *g_pp_rt; static uint32_t g_pp_rtseq;
static uint32_t *g_rtx_seq, *g_drx_seq; static uint32_t g_rtx_n, g_rtx_cap, g_drx_n, g_drx_cap;   // This submit's SET_RT / draw sequence number tables (used by the pre-screen of the readable tile merge)
static void seq_push(uint32_t **v, uint32_t *n, uint32_t *cap, uint32_t x) { if (*n == *cap) { *cap = *cap ? *cap * 2 : 1024; *v = realloc(*v, *cap * sizeof **v); } (*v)[(*n)++] = x; }
static void sdx_ev_add(sdx_ev e) { if (sdx_bucket(e.ser, 0) < 0) return; if (g_sdx_n == g_sdx_cap) { g_sdx_cap = g_sdx_cap ? g_sdx_cap * 2 : 4096; g_sdx = realloc(g_sdx, g_sdx_cap * sizeof *g_sdx); g_sdx_tmp = realloc(g_sdx_tmp, g_sdx_cap * sizeof *g_sdx); } g_sdx[g_sdx_n++] = e; }
static void sdx_lvsl(const view_t *v, int hv, uint16_t *lv, uint16_t *sl) { *lv = (uint16_t)(hv && v->set_levels ? v->base_level : 0); *sl = (uint16_t)(hv && v->set_layers ? v->min_layer : 0); }
static void sdx_walk(const uint8_t *p, int depth) {
    if (depth > 64) { g_sdx_ok = 0; return; }
    for (uint32_t guard = 0; guard < 4000000 && p && g_sdx_ok; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        if (g_sdx_nop == g_sdx_opcap) { g_sdx_opcap = g_sdx_opcap ? g_sdx_opcap * 2 : 16384; g_sdx_op = realloc(g_sdx_op, g_sdx_opcap * sizeof *g_sdx_op); }
        uint32_t seq = g_sdx_nop; g_sdx_op[g_sdx_nop++] = p;
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); sdx_walk((const uint8_t *)t, depth + 1); break; }
            case OP_SET_RT: { const p_set_rt *r = a; g_pp_rt = r; g_pp_rtseq = seq; seq_push(&g_rtx_seq, &g_rtx_n, &g_rtx_cap, seq);
                for (int k = 0; k < r->n && k < 8; k++) if (r->color[k]) sdx_bucket(r->color[k]->serial, 1);
                if (r->depth) sdx_bucket(r->depth->serial, 1);
                break; }
            case OP_CLEAR_COLOR: { const p_clear_color *c = a; const p_set_rt *r = g_pp_rt;
                if (!r || (c->mask & 0xf) != 0xf || c->index < 0 || c->index >= r->n || c->index >= 8 || !r->color[c->index]) break;
                uint16_t lv, sl; sdx_lvsl(&r->cview[c->index], r->has_cview[c->index], &lv, &sl); int last = -1;   // When the same subresource is attached at several indices, the last one counts
                for (int k = 0; k < r->n && k < 8; k++) { uint16_t l2, s2; if (!r->color[k] || r->color[k]->serial != r->color[c->index]->serial) continue; sdx_lvsl(&r->cview[k], r->has_cview[k], &l2, &s2); if (l2 == lv && s2 == sl) last = k; }
                if (last == c->index) sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->color[c->index]->serial, p, lv, sl, SDX_CLEAR, 0, 0, 0});
                break; }
            case OP_CLEAR_DS: { const p_clear_ds *c = a; const p_set_rt *r = g_pp_rt; if (!r || !r->depth) break;
                uint16_t lv, sl; sdx_lvsl(&r->dview, r->has_dview, &lv, &sl);
                if (c->depth_mask) sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->depth->serial, p, lv, sl, SDX_CLEAR, 1, !r->has_dview, 0});
                if ((c->stencil_mask & 0xff) == 0xff) sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->depth->serial, p, lv, sl, SDX_CLEAR, 2, !r->has_dview, 0});
                break; }
            case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_IMG: { p_bind_handles one; const p_bind_handles *b = bind_rec(h.op, a, &one); bh_fresh(b); int base = h.op == OP_BIND_IMG ? NTEX : 0, lim = h.op == OP_BIND_IMG ? 32 : NTEX;
                if (b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) g_pp_slot[b->stage][base + b->first + k] = h_ser(b->h[k]);
                break; }
            case OP_BIND_PROGRAM: g_pp_prog = ((const p_bind_program *)a)->prog; break;
            case OP_DRAW: case OP_DRAW_IDX: case OP_DISPATCH: {
                if (h.op != OP_DISPATCH) seq_push(&g_drx_seq, &g_drx_n, &g_drx_cap, seq);
                __unsafe_unretained NvProg *pr = g_pp_prog ? prog_get(g_pp_prog) : nil;
                if (!pr) { for (int st = 0; st < 6; st++) for (int k = 0; k < NTEX + 32; k++) if (g_pp_slot[st][k]) sdx_ev_add((sdx_ev){seq, 0, g_pp_slot[st][k], p, 0, 0, SDX_READ, 0, 0, 1}); }
                else for (int st = 0; st < 6; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || (h.op == OP_DISPATCH) != (st == 5)) continue;
                    for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; int k = b->kind == NVN_RB_IMG ? NTEX + b->reg : (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) ? b->reg : -1;
                        if (k < 0 || k >= NTEX + 32 || sh->stage < 0 || sh->stage >= 6) continue; uint64_t ser = g_pp_slot[sh->stage][k];
                        if (ser) sdx_ev_add((sdx_ev){seq, 0, ser, p, 0, 0, SDX_READ, 0, 0, 1}); } }
                const p_set_rt *r = g_pp_rt;
                if (h.op != OP_DISPATCH && r) {
                    for (int k = 0; k < r->n && k < 8; k++) if (r->color[k]) { uint16_t lv, sl; sdx_lvsl(&r->cview[k], r->has_cview[k], &lv, &sl); sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->color[k]->serial, p, lv, sl, SDX_ATT, 0, 0, 2}); }
                    if (r->depth) { uint16_t lv, sl; sdx_lvsl(&r->dview, r->has_dview, &lv, &sl); sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->depth->serial, p, lv, sl, SDX_ATT, 8, !r->has_dview, 2}); }
                }
                break; }
            case OP_COPY_T2T: { const p_copy_t2t *c = a; if (c->src) sdx_ev_add((sdx_ev){seq, 0, c->src->serial, p, 0, 0, SDX_NEED, 0, 0, 3}); if (c->dst) sdx_ev_add((sdx_ev){seq, 0, c->dst->serial, p, 0, 0, SDX_NEED, 0, 0, 3}); break; }
            case OP_COPY_T2B: case OP_COPY_B2T: { const p_copy_b2t *c = a; if (c->tex) sdx_ev_add((sdx_ev){seq, 0, c->tex->serial, p, 0, 0, SDX_NEED, 0, 0, 3}); break; }
            case OP_CLEAR_TEX: { const p_clear_tex *c = a; if (c->tex) sdx_ev_add((sdx_ev){seq, 0, c->tex->serial, p, 0, 0, SDX_NEED, 0, 0, 4}); break; }
            default: break;
        }
        p += h.size;
    }
}
void mtl_sub_index(const uint64_t *h, int n) {   // Start of submit (hle_nvn.c QueueSubmitCommands): build this submit's store lookahead index
    g_xv_ntouch = 0; g_sdx_n = g_sdx_nop = 0; g_rtx_n = g_drx_n = 0; g_exec_seq = 0; g_sdx_ok = tbdr_on();
    if (!g_sdx_ok) { g_xv_n = 0; g_xv_block = 1; return; }   // Index unavailable: clear the ledger and add nothing in this submit (the next touch cannot be verified)
    g_xv_block = 0;
    memset(g_sdx_hk, 0, sizeof g_sdx_hk); g_sdx_nb = 0;
    for (int i = 0; i < g_xv_n; i++) sdx_bucket(g_xv[i].ser, 1);   // Textures in the ledger enter the candidate set: otherwise, when one is only sampled and not used as an attachment in this submit, its read events are not indexed and the fallback cannot see them
    if (R.has_rt) { for (int k = 0; k < R.rt.n && k < 8; k++) if (R.rt.color[k]) sdx_bucket(R.rt.color[k]->serial, 1); if (R.rt.depth) sdx_bucket(R.rt.depth->serial, 1); }   // The targets at the start of the submit (the first draw may reopen the render pass without going through SetRenderTargets)
    g_pp_prog = S.prog; g_pp_rt = NULL; g_pp_rtseq = SDX_NONE;
    for (int st = 0; st < 6; st++) { for (int k = 0; k < NTEX; k++) g_pp_slot[st][k] = S.tex[st][k] ? h_ser(S.tex[st][k]) : 0;
                                     for (int k = 0; k < 32; k++) g_pp_slot[st][NTEX + k] = S.img[st][k] ? h_ser(S.img[st][k]) : 0; }
    for (int i = 0; i < n && g_sdx_ok; i++) sdx_walk((const uint8_t *)h[i], 0);
    if (!g_sdx_ok) return;
    // Counting sort into buckets: events are appended in sequence-number order, and within the same sequence number READ precedes ATT (the same draw is judged for reads first, then attachments), so after a stable scatter each bucket is ordered
    memset(g_sdx_bcnt, 0, (g_sdx_nb + 1) * sizeof *g_sdx_bcnt);
    for (uint32_t i = 0; i < g_sdx_n; i++) g_sdx_bcnt[sdx_bucket(g_sdx[i].ser, 0)]++;
    for (uint32_t b = 0, acc = 0; b <= g_sdx_nb; b++) { g_sdx_bst[b] = acc; acc += b < g_sdx_nb ? g_sdx_bcnt[b] : 0; }
    for (uint32_t b = 0; b < g_sdx_nb; b++) g_sdx_bcnt[b] = g_sdx_bst[b];
    for (uint32_t i = 0; i < g_sdx_n; i++) g_sdx_tmp[g_sdx_bcnt[sdx_bucket(g_sdx[i].ser, 0)]++] = g_sdx[i];
    sdx_ev *t = g_sdx; g_sdx = g_sdx_tmp; g_sdx_tmp = t;
    xv_forward();
}
static void sdx_seq(const uint8_t *p) {   // Every operation in mtl_exec: check the sequence number against the index (on mismatch invalidate, and store everything for the rest)
    if (!g_sdx_ok) return;
    if (g_exec_seq >= g_sdx_nop || g_sdx_op[g_exec_seq] != p) { RED_ONCE(40, "写回前瞻索引与回放顺序不符（序号 %u / %u），本次提交余下通道一律写回", g_exec_seq, g_sdx_nop); g_sdx_ok = 0; return; }
    g_exec_seq++;
}
static int sdx_decide(uint8_t st[9]) {   // st[i]: 2 redundant; otherwise why << 2 (the reason it is needed, same as sd_need). Returns 0 = index unavailable
    if (!g_sdx_ok || !g_exec_cur || !g_exec_seq || g_sdx_op[g_exec_seq - 1] != g_exec_cur) return 0;
    uint32_t P = g_exec_seq - 1;
    for (int i = 0; i < g_nsd; i++) { const sd_att *t = &g_sd[i]; st[i] = 5 << 2;
        int b = sdx_bucket(t->ser, 0); if (b < 0) continue;   // Not in the candidate set: no related event at all in this submit -> undecided after scanning to the end, needed
        uint32_t lo = g_sdx_bst[b], end = g_sdx_bst[b + 1], hi = end;   // First seq >= P in the bucket
        while (lo < hi) { uint32_t m = (lo + hi) / 2; if (g_sdx[m].seq < P) lo = m + 1; else hi = m; }
        for (uint32_t j = lo; j < end; j++) { const sdx_ev *e = &g_sdx[j];
            if (g_hoist_n && hoist_skipped(e->op)) continue;
            if (e->kind == SDX_READ || e->kind == SDX_NEED) { st[i] = (uint8_t)(e->why << 2); break; }
            if (e->rtseq == SDX_NONE || e->rtseq < P || (g_hoist_n && hoist_skipped(g_sdx_op[e->rtseq]))) continue;   // This target was set before the render pass ended: not re-set as an attachment after the render pass ended, so it does not count
            int hit = t->depth ? (e->kind == SDX_ATT ? e->plane == 8 : e->plane == t->depth) && (e->anyls || (e->lv == t->lv && e->sl == t->sl))
                               : e->plane == 0 && e->lv == t->lv && e->sl == t->sl;
            if (!hit) continue;
            st[i] = e->kind == SDX_CLEAR ? 2 : (uint8_t)(2 << 2); break; }
    }
    return 1;
}
static void xv_forward(void) {
    if (!g_xv_n) return;
    if (!g_sdx_ok) { g_xv_n = 0; g_xv_block = 1; return; }
    for (int i = 0; i < g_xv_n;) {
        xv_ent *e = &g_xv[i]; int b = sdx_bucket(e->ser, 0), action = 0;
        if (b >= 0) for (uint32_t j = g_sdx_bst[b]; j < g_sdx_bst[b + 1]; j++) { sdx_ev *v = &g_sdx[j];
            if (v->kind == SDX_READ || v->kind == SDX_NEED) { xv_ent copy = *e; xv_violation(&copy, "提交前瞻读/需求"); action = 2; break; }
            if (v->lv != e->lv || v->sl != e->sl) continue;
            if (v->kind == SDX_CLEAR) { xv_good(e); action = 1; break; }
            if (v->kind == SDX_ATT) break;
        }
        if (!action) i++;
    }
}
static void rt_sub(const p_set_rt *r, int k, uint64_t *ser, int *lv, int *sl) {   // Subresource of attachment k (0–7 color, 8 depth)
    const tex_t *t = k == 8 ? r->depth : r->color[k]; const view_t *v = k == 8 ? &r->dview : &r->cview[k]; int hv = k == 8 ? r->has_dview : r->has_cview[k];
    *ser = t ? t->serial : 0; *lv = hv && v->set_levels ? v->base_level : 0; *sl = hv && v->set_layers ? v->min_layer : 0;
}
static void hoist_try(void) {
    if (g_hoist_n || !g_exec_next) return;
    typedef struct { const uint8_t *rt_op; const p_set_rt *rt; uint8_t clear_c[8]; MTLClearColor cc[8]; uint8_t clear_d, clear_s; double cd; uint32_t cs;
                     p_scissor sc; int has_sc; const uint8_t *ops[8]; int nops; } grp;
    grp G[4]; int ng = 0, in_grp = 0; p_scissor sc = R.sc; int has_sc = R.has_sc;
    // The binding set needed for "the cleared texture is not on any slot": the table is looked up only when a hoistable clear group is really found and we are back to this render pass's targets (the vast majority of render passes never get that far).
    // Handles bound within the scan range are noted first and converted to texture serials together with the current slots at lookup time; the scan only reads commands and the current slots S do not change meanwhile, so the result is the same as checking at the start
    uint64_t bh[512]; int nbh = 0;
    const uint8_t *p = g_exec_next;
    for (int guard = 0; guard < 200000; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        switch (h.op) {
            case OP_END: case OP_CALL: case OP_COPY_T2T: case OP_COPY_B2T: case OP_COPY_T2B: case OP_CLEAR_TEX: case OP_DISPATCH: case OP_RENDER_COND: return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_DRAW: case OP_DRAW_IDX: if (in_grp) return; break;
            case OP_SCISSOR: sc = *(const p_scissor *)a; has_sc = 1; break;
            case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_IMG: { p_bind_handles one; const p_bind_handles *b = bind_rec(h.op, a, &one); bh_fresh(b); for (int k = 0; k < b->n && k < 32 && nbh < 512; k++) if (b->h[k]) bh[nbh++] = b->h[k]; break; }
            case OP_CLEAR_COLOR: case OP_CLEAR_DS: {
                if (!in_grp || G[ng - 1].nops >= 8) return;
                grp *g = &G[ng - 1];
                if (h.op == OP_CLEAR_COLOR) { const p_clear_color *c = a;
                    if (c->index < 0 || c->index >= g->rt->n || !g->rt->color[c->index] || c->mask != 0xf) return;
                    const tex_t *t = g->rt->color[c->index];
                    if (has_sc && !(sc.x <= 0 && sc.y <= 0 && sc.x + sc.w >= (int)t->d.width && sc.y + sc.h >= (int)t->d.height)) return;
                    g->clear_c[c->index] = 1; g->cc[c->index] = MTLClearColorMake(c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]); }
                else { const p_clear_ds *c = a; if (!g->rt->depth) return;
                    if (c->depth_mask) { g->clear_d = 1; g->cd = c->depth; } if (c->stencil_mask) { g->clear_s = 1; g->cs = (uint32_t)c->stencil; } }
                g->sc = sc; g->has_sc = has_sc; g->ops[g->nops++] = p; break; }
            case OP_SET_RT: { const p_set_rt *r = a;
                if (rt_same(&R.rt, r)) {   // Back to this render pass's targets: execute if hoisting holds
                    if (!ng) return;
                    uint64_t bound[512]; int nb = 0;   // The order of the original "check at the start": the current slots (stage x textures 128 + images 32), followed by those bound within the scan range
                    for (int st = 0; st < 6; st++) { for (int k = 0; k < NTEX && nb < 512; k++) if (S.tex[st][k]) bound[nb++] = h_ser(S.tex[st][k]);
                                                     for (int k = 0; k < 32 && nb < 512; k++) if (S.img[st][k]) bound[nb++] = h_ser(S.img[st][k]); }
                    for (int k = 0; k < nbh && nb < 512; k++) bound[nb++] = h_ser(bh[k]);
                    for (int gi = 0; gi < ng; gi++) {   // Disjoint and not bound
                        grp *g = &G[gi]; if (!g->nops) return;
                        for (int k = 0; k < 9; k++) { if (k < 8 ? !g->clear_c[k] : !(g->clear_d || g->clear_s)) continue;
                            uint64_t ser; int lv, sl; rt_sub(g->rt, k, &ser, &lv, &sl); if (!ser) return;
                            for (int j = 0; j < nb; j++) if (bound[j] == ser) return;
                            for (int m = 0; m < 9; m++) { uint64_t s2; int l2, sl2; if (m < 8 && m >= R.rt.n) continue; rt_sub(&R.rt, m, &s2, &l2, &sl2);
                                if (s2 == ser && l2 == lv && sl2 == sl) return; } } }
                    // Register all skip entries first, then execute: when a hoisted render pass ends the store lookahead scans forward, and the clears at their original positions must already be in the skip set
                    for (int gi = 0; gi < ng; gi++) { g_hoist_skip[g_hoist_n++] = G[gi].rt_op; for (int k = 0; k < G[gi].nops; k++) g_hoist_skip[g_hoist_n++] = G[gi].ops[k]; }
                    p_set_rt rt0 = R.rt; p_scissor sc0 = R.sc; int hs0 = R.has_sc;   // This render pass's targets, scissor and pending clears: restored as they were after execution
                    uint8_t cc0f[8]; MTLClearColor cc0[8]; uint8_t cd0f = R.clear_d, cs0f = R.clear_s; double cd0 = R.cd; uint32_t cs0 = R.cs;
                    memcpy(cc0f, R.clear_c, 8); memcpy(cc0, R.cc, sizeof cc0);
                    for (int gi = 0; gi < ng; gi++) {   // Execute: temporarily swap in that group's targets and clears and open a clear-only render pass
                        grp *g = &G[gi];
                        R.rt = *g->rt; memcpy(R.clear_c, g->clear_c, 8); memcpy(R.cc, g->cc, sizeof R.cc); R.clear_d = g->clear_d; R.clear_s = g->clear_s; R.cd = g->cd; R.cs = g->cs;
                        R.sc = g->sc; R.has_sc = g->has_sc;
                        PD("H %llu 前提纯清除通道（原位置在本通道之后）", g_stat_pass + 1);
                        rp_begin_ex(1); rp_end();
                    }
                    R.rt = rt0; R.sc = sc0; R.has_sc = hs0; memcpy(R.clear_c, cc0f, 8); memcpy(R.cc, cc0, sizeof cc0); R.clear_d = cd0f; R.clear_s = cs0f; R.cd = cd0; R.cs = cs0;
                    if (!g_hoist_groups) MLOG("纯清除通道前提：首例 %d 组（通道 %llu 前）", ng, g_stat_pass + 1);
                    g_hoist_groups += (uint64_t)ng;
                    return;
                }
                if (in_grp && !G[ng - 1].nops) return;   // The previous group has no clear (only set targets): not recognized
                if (ng >= 4) return;
                memset(&G[ng], 0, sizeof G[ng]); G[ng].rt_op = p; G[ng].rt = r; ng++; in_grp = 1; break; }
            default: break;
        }
        p += h.size;
    }
}
// ---------- Readable tile render pass merge (merge B step 2) ----------
// When a draw of the later pass samples, at the same pixel, a color attachment written by the earlier pass, the two passes are merged into one render pass: what the earlier pass wrote stays in the tile,
// and the later pass reads it directly with the readable tile version (<key>_bf, function-constant specialization), instead of first landing in main memory and being read back just to be sampled.
// Planning (fuse_try, at the start of the earlier pass) does only structural checks; the closed-form proof of same-pixel addressing is done at every draw of the later pass (fz_p2_draw), and if it cannot be proven the merged render pass is ended and
// a normal render pass is reopened with the later pass's original targets (the earlier pass's result is stored as usual, equivalent to not merging).
static void fz_unskip(void) { for (int i = 0; i < g_fz.nskip; i++) for (int j = 0; j < g_hoist_n; j++) if (g_hoist_skip[j] == g_fz.skip[i]) { g_hoist_skip[j] = g_hoist_skip[--g_hoist_n]; break; } }
static int fz_reuse_op(const uint8_t *p) { for (int i = 0; i < g_fz.nreuse; i++) if (g_fz.reuse[i] == p) return 1; return 0; }
// Diagnostic DM_NVN_FUSE_CHECK=1: in-run double rendering. For the later pass's draws that can be proven, keep a state snapshot per draw; after the merged render pass ends, make a copy of the attachments the later pass wrote (the merge path's result),
// then turn off all lookahead and redraw one by one through the normal path with the later pass's original targets and original clears, and compare the two byte by byte (same frame, same input, no dependence on cross-run determinism). The redraw result is left on the attachment (no effect if identical)
static int fz_check_on(void) { static int v = -1; if (v < 0) v = getenv("DM_NVN_FUSE_CHECK") != NULL; return v; }
typedef struct { int idx; p_draw dw; p_draw_idx di; p_scissor sc; int has_sc; void *S_; } fz_snap;
static fz_snap g_fzs[8]; static int g_nfzs, g_fzs_bad, g_fz_checking; static uint64_t g_fzc_same, g_fzc_diff;
static void draw(const p_draw *dw, const p_draw_idx *di);
static void fz_snap_take(const p_draw *dw, const p_draw_idx *di) {
    if (g_nfzs >= 8) { g_fzs_bad = 1; return; }
    fz_snap *z = &g_fzs[g_nfzs++]; z->idx = di != NULL; if (dw) z->dw = *dw; if (di) z->di = *di; z->sc = R.sc; z->has_sc = R.has_sc;
    z->S_ = malloc(sizeof S); memcpy(z->S_, &S, sizeof S);
}
static void fz_check_run(const fuse_t *fz) {
    if (!g_nfzs || g_fzs_bad) { if (g_fzs_bad) MLOG("【红】局内双渲染：后一个通道绘制超过 8 次，未比"); goto out; }
    {
    id<MTLTexture> att[8], scr[8]; uint64_t sers[8]; int na = 0;
    // fz_end has already restored R.rt from the physical union to the later pass's logical targets; wrote is still registered by physical slot and must be aligned through p2map.
    for (int j = 0; j < fz->rt2.n && j < 8; j++) if (fz->p2map[j] >= 0 && fz->wrote[fz->p2map[j]] && fz->rt2.color[j]) {
        id<MTLTexture> t = tex_mtl(fz->rt2.color[j]); if (!t) continue;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:t.width height:t.height mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> c = [g_dev newTextureWithDescriptor:d]; if (!c) continue;
        enc_b_copy_texture(blit(), t, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(t.width, t.height, 1), c, 0, 0, MTLOriginMake(0, 0, 0));
        att[na] = t; scr[na] = c; sers[na] = fz->rt2.color[j]->serial; na++; }
    if (!na) { MLOG("【红】局内双渲染：已留 %d 次绘制快照，但没有可比附件", g_nfzs); goto out; }
    static typeof(S) Ssave; memcpy(&Ssave, &S, sizeof S); typeof(R) Rsave = R; const uint8_t *en0 = g_exec_next;
    g_fz_checking = 1; g_exec_next = NULL;   // Turn off all lookahead during the redraw (continuation / hoisting / merging / depth-stencil on demand): normal path
    R.rt = fz->rt2; memcpy(R.clear_c, fz->clr2, 8); memcpy(R.cc, fz->cc2, sizeof R.cc); R.clear_d = fz->cd2f; R.clear_s = fz->cs2f; R.cd = fz->cd2; R.cs = fz->cs2;
    for (int i = 0; i < g_nfzs; i++) { fz_snap *z = &g_fzs[i]; memcpy(&S, z->S_, sizeof S); args_dirty_all(); R.sc = z->sc; R.has_sc = z->has_sc; g_pso_dirty = g_dss_dirty = 1;
        draw(z->idx ? NULL : &z->dw, z->idx ? &z->di : NULL); }
    rp_end();
    memcpy(&S, &Ssave, sizeof S); args_dirty_all(); R = Rsave; g_exec_next = en0; g_fz_checking = 0; g_pso_dirty = g_dss_dirty = 1;
    for (int a = 0; a < na; a++) {
        id<MTLTexture> t = att[a], c = scr[a]; NSUInteger W = t.width, H = t.height, bpp = (NSUInteger)(att_bytes(t, 0) / (W * H));
        id<MTLBuffer> b1 = [g_dev newBufferWithLength:W * H * bpp options:MTLResourceStorageModeShared], b2 = [g_dev newBufferWithLength:W * H * bpp options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> b = blit();
        enc_b_copy_texture_buffer(b, c, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(W, H, 1), b1, 0, W * bpp, W * H * bpp);
        enc_b_copy_texture_buffer(b, t, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(W, H, 1), b2, 0, W * bpp, W * H * bpp);
        unsigned long long ser = sers[a], pass = g_stat_pass, frame = atomic_load(&g_cur_frame); NSUInteger nb = W * H * bpp;
        [g_commit_blocks addObject:^{
            const uint8_t *x = b1.contents, *y = b2.contents; uint64_t dpx = 0, maxd = 0;
            for (NSUInteger k = 0; k < nb; k += bpp) if (memcmp(x + k, y + k, bpp)) { dpx++; for (NSUInteger q = 0; q < bpp; q++) { uint64_t dd = (uint64_t)abs((int)x[k + q] - (int)y[k + q]); if (dd > maxd) maxd = dd; } }
            if (dpx) { g_fzc_diff++; MLOG("【红】局内双渲染：帧 %llu 通道 %llu 附件 ser=%llu 合并 vs 普通 不同像素 %llu / %lu（逐字节最大差 %llu）", frame, pass, ser, (unsigned long long)dpx, (unsigned long)(nb / bpp), (unsigned long long)maxd); }
            else { g_fzc_same++; MLOG("局内双渲染：帧 %llu 通道 %llu 附件 ser=%llu 合并 vs 普通 逐字节相同（%lu 像素）", frame, pass, ser, (unsigned long)(nb / bpp)); }
            if ((g_fzc_same + g_fzc_diff) % 300 == 0) MLOG("局内双渲染累计：逐字节相同 %llu 次、不同 %llu 次", (unsigned long long)g_fzc_same, (unsigned long long)g_fzc_diff);
        }];
    }
    }
out:
    for (int i = 0; i < g_nfzs; i++) free(g_fzs[i].S_);
    g_nfzs = 0; g_fzs_bad = 0;
}
static void fz_end(void) {   // End of a merged render pass (called by rp_end): interrupted before entering the later pass -> retract the skip entries and restore the targets to the earlier pass's; otherwise restore to the later pass's
    if (!g_fz.active) return;
    if (g_fz.phase == 1) { fz_unskip(); R.rt = g_fz.rt1; } else R.rt = g_fz.rt2;
    g_fz.active = 0;
    if (g_fz.phase == 2 && fz_check_on() && !g_fz_checking) { fuse_t fz = g_fz; R.rt = fz.rt2; fz_check_run(&fz); }
    else { for (int i = 0; i < g_nfzs; i++) free(g_fzs[i].S_); g_nfzs = 0; g_fzs_bad = 0; }
}
static int sub_eq(const p_set_rt *a, int ka, const p_set_rt *b, int kb) { uint64_t s1, s2; int l1, l2, x1, x2; rt_sub(a, ka, &s1, &l1, &x1); rt_sub(b, kb, &s2, &l2, &x2); return s1 && s1 == s2 && l1 == l2 && x1 == x2; }
static int prog_samples_ser(const prog_t *pg, uint64_t tex[6][NTEX], uint64_t img[6][32], uint64_t ser) {   // Whether this texture is bound in any texture / image / typed buffer slot declared by the program's reflection
    __unsafe_unretained NvProg *pr = pg ? prog_get(pg) : nil; if (!pr) return 1;
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || sh->stage >= 6) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 ? img[sh->stage][b->reg] : 0; else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = b->reg < NTEX ? tex[sh->stage][b->reg] : 0; else continue;
            if (h && h_ser(h) == ser) return 1; } }
    return 0;
}
static int g_defer_env_ab, g_defer_env_off;   // Environment switches: read once at the start of each submit (defer_reset); the hot path does not call getenv
static void defer_env(void) { const char *x = getenv("DM_NVN_DEFER_AB"), *e = getenv("DM_NVN_DEFER"); g_defer_env_ab = x && !strcmp(x, "1"); g_defer_env_off = e && !strcmp(e, "0"); }
static int defer_on(void) {
    if (!g_defer_env_ab) return tbdr_on() && !g_defer_env_off;
    if (g_defer_ab < 0) { g_defer_ab = 0; g_defer_ab_on = 0; }
    return tbdr_on() && (g_defer_ab || g_defer_ab_on); }
static int defer_handle_sub(uint64_t h, const p_set_rt *r, int k, int cover) {
    int tid = (int)(h & 0xfffff); if (!tid || tid >= g_ntreg || !g_treg[tid].ok) return 0;
    uint64_t ser; int lv, sl; rt_sub(r, k, &ser, &lv, &sl); if (!ser || g_treg[tid].t.serial != ser) return 0;
    if (!g_treg[tid].has_v || !g_treg[tid].v.set_levels) return cover;
    view_t v = g_treg[tid].v; int n = v.nlevels > 0 ? v.nlevels : 1;
    return lv >= v.base_level && lv < v.base_level + n && (!v.set_layers || (sl >= v.min_layer && sl < v.min_layer + (v.nlayers > 0 ? v.nlayers : 1)));
}
static int defer_prog_hits(const prog_t *pg, uint64_t tex[6][NTEX], uint64_t img[6][32], const p_set_rt *r, int color_only, int cover) {
    __unsafe_unretained NvProg *pr = pg ? prog_get(pg) : nil; if (!pr) return 1;
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || sh->stage < 0 || sh->stage >= 6) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h = 0;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 ? img[sh->stage][b->reg] : 0;
            else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = b->reg < NTEX ? tex[sh->stage][b->reg] : 0;
            else continue;
            if (!h) continue;
            for (int k = 0; k < (color_only ? r->n : 9); k++) if ((color_only ? r->color[k] : (k < 8 ? r->color[k] : r->depth)) && defer_handle_sub(h, r, k, cover)) return 1;
        }
    }
    return 0;
}
static void defer_collect_reads(defer_t *d, const prog_t *pg, uint64_t tex[6][NTEX], uint64_t img[6][32]) {
    __unsafe_unretained NvProg *pr = pg ? prog_get(pg) : nil; if (!pr) { d->nreads = 513; return; }
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || sh->stage < 0 || sh->stage >= 6) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h = 0;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 ? img[sh->stage][b->reg] : 0;
            else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = b->reg < NTEX ? tex[sh->stage][b->reg] : 0;
            else continue;
            if (!h) continue; int seen = 0; for (int j = 0; j < d->nreads; j++) seen |= d->p_reads[j] == h;
            if (!seen && d->nreads < 512) d->p_reads[d->nreads++] = h;
        }
    }
}
static int defer_rt_hits_reads(const p_set_rt *r, int idx) {
    if (idx < 0 || idx >= r->n || !r->color[idx]) return 0;
    for (int i = 0; i < g_defer.nreads; i++) if (defer_handle_sub(g_defer.p_reads[i], r, idx, 1)) return 1;
    return 0;
}
static int defer_q_superset(const p_set_rt *q) {
    memset(g_defer.lmap, 0xff, sizeof g_defer.lmap);
    for (int i = 0; i < g_defer.p_rt.n; i++) if (g_defer.p_rt.color[i]) { int found = 0;
        // The indices must be the same: a normal fragment function's color(n) output is written to physical attachment n, and lmap only changes the write mask / blend and cannot move the output (only the _bf version with output mapping constants can).
        // With different indices, deferral would write P's output into a different attachment of Q.
        if (i < q->n && q->color[i] && sub_eq(&g_defer.p_rt, i, q, i)) { g_defer.lmap[i] = (uint8_t)i; found = 1; }
        if (!found) return 0;
    }
    return 1;
}
static int defer_take(const p_draw *dw, const p_draw_idx *di) {
    if (g_defer.n >= 64) { g_defer.reason = 20; return 0; }
    defer_snap *z = &g_defer.snap[g_defer.n++]; z->idx = di != NULL; if (dw) z->dw = *dw; if (di) z->di = *di;
    z->sc = R.sc; z->has_sc = R.has_sc; z->S_ = malloc(sizeof S); memcpy(z->S_, &S, sizeof S); for (int k = 0; k < 8; k++) z->pm[k] = (uint8_t)k; g_defer_draws++; return 1;
}
static void defer_release(void) { defer_release_one(&g_defer); }
static void defer_env(void);
static void defer_reset(void) { defer_clear_all(); g_defer_replaying = 0; defer_env(); }
static void defer_apply_p_clear(void) {
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
    for (int j = 0; j < g_defer.q_rt.n; j++) { int i = g_defer.lmap[j]; if (i >= 0 && g_defer.clear_c[i]) { R.clear_c[j] = 1; R.cc[j] = g_defer.cc[i]; } }
    if (g_defer.clear_d) { R.clear_d = 1; R.cd = g_defer.cd; } if (g_defer.clear_s) { R.clear_s = 1; R.cs = g_defer.cs; }
}
static int defer_begin_q(const uint8_t *op) {
    int found = 0, first = -1;
    for (int i = 0; i < 4; i++) if (g_defer_slots[i].active && !g_defer_slots[i].at_q && g_defer_slots[i].q_op == op) { found = 1; if (first < 0) first = i; }
    if (!found) return 0;
    rp_flush();
    g_defer_cur = &g_defer_slots[first]; R.rt = g_defer.q_rt; R.has_rt = 1;
    for (int i = 0; i < 4 && !R.rt.depth; i++) { const defer_t *d = &g_defer_slots[i]; if (d->active && !d->at_q && d->q_op == op && d->q_rt.depth) { R.rt.depth = d->q_rt.depth; R.rt.dview = d->q_rt.dview; R.rt.has_dview = d->q_rt.has_dview; } }   // Attach the depth-stencil needed by any group (the depths of groups with the same Q have been checked compatible)
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
    for (int i = 0; i < 4; i++) { defer_t *d = &g_defer_slots[i]; if (!d->active || d->at_q || d->q_op != op) continue;
        for (int j = 0; j < d->q_rt.n; j++) { int k = d->lmap[j]; if (k >= 0 && d->clear_c[k]) { R.clear_c[j] = 1; R.cc[j] = d->cc[k]; } }
        if (d->clear_d) { R.clear_d = 1; R.cd = d->cd; } if (d->clear_s) { R.clear_s = 1; R.cs = d->cs; }
        d->at_q = 1; g_defer_natq++;
    }
    return 1;
}
static void defer_replay(void) {
    if (g_defer_replaying) return;
    int any = 0; for (int i = 0; i < 4; i++) if (g_defer_slots[i].active && g_defer_slots[i].at_q) any = 1;
    if (!any) return;
    typeof(S) save; memcpy(&save, &S, sizeof S); p_scissor sc = R.sc; int has_sc = R.has_sc; int vis = g_vis_counting;
    const uint8_t *en0 = g_exec_next; g_defer_replaying = 1; g_exec_next = NULL;
    for (int gi, after = 0; (gi = defer_next(1, after)) >= 0;) { defer_t *d = &g_defer_slots[gi]; after = d->born;   // In creation order: the groups' P ranges do not overlap and are arranged in sequence, so the group order is the original draw order
        g_defer_cur = d; if (d->reset_between) g_vis_counting = 0;
        { static int once; if (!once++) MLOG("推迟合并通道：首例 通道 %llu，组 %d 回放绘制 %d 次", g_stat_pass, gi, d->n); }   // With multiple groups, several groups may already have been created before the first replay, so it cannot be judged by g_defer_n == 1
        for (int i = 0; i < d->n; i++) { defer_snap *z = &d->snap[i]; static uint8_t lm[8]; for (int j = 0; j < 8; j++) lm[j] = d->lmap[j] == 0xff || d->lmap[j] >= 8 ? 0xff : z->pm[d->lmap[j]];
            memcpy(&S, z->S_, sizeof S); args_dirty_all(); R.sc = z->sc; R.has_sc = z->has_sc; g_pso_dirty = g_dss_dirty = 1; g_pso_lmap = lm; draw(z->idx ? NULL : &z->dw, z->idx ? &z->di : NULL); }
        defer_release_one(d);
    }
    g_vis_counting = vis; memcpy(&S, &save, sizeof S); args_dirty_all(); R.sc = sc; R.has_sc = has_sc; g_pso_lmap = NULL; g_pso_fsfn = nil; memset(&g_pso_fz, 0, sizeof g_pso_fz); g_pso_dirty = g_dss_dirty = 1; g_exec_next = en0; g_defer_replaying = 0; g_defer_cur = &g_defer_slots[0];
}
static void defer_fallback(void) {
    if (g_defer_replaying || !defer_has_active()) return;
    g_defer_fallbacks++; MLOG("【红】推迟合并通道：提交结束仍有未回放组，按各组 P 原目标兜底");
    if (g_enc) { enc_r_end(g_enc); g_enc = nil; }
    typeof(S) save; memcpy(&save, &S, sizeof S); p_set_rt rt = R.rt; g_defer_replaying = 1;
    for (int gi, after = 0; (gi = defer_next(0, after)) >= 0;) { defer_t *d = &g_defer_slots[gi]; after = d->born; g_defer_cur = d; R.rt = d->p_rt; R.has_rt = 1; memcpy(R.clear_c, d->clear_c, 8); memcpy(R.cc, d->cc, sizeof R.cc);
        R.clear_d = d->clear_d; R.clear_s = d->clear_s; R.cd = d->cd; R.cs = d->cs;
        for (int i = 0; i < d->n; i++) { defer_snap *z = &d->snap[i]; memcpy(&S, z->S_, sizeof S); args_dirty_all(); R.sc = z->sc; R.has_sc = z->has_sc; g_pso_dirty = g_dss_dirty = 1; g_pso_lmap = z->pm; draw(z->idx ? NULL : &z->dw, z->idx ? &z->di : NULL); }
        rp_end(); defer_release_one(d);
    }
    memcpy(&S, &save, sizeof S); args_dirty_all(); R.rt = rt; g_defer_replaying = 0; g_defer_cur = &g_defer_slots[0];
}
static const uint8_t *defer_cont(int *ci) {   // The next segment after a list ends (within the same submit): first the caller return stack, then the remaining handles of this submit; returns NULL when there are none left
    for (;;) { int k = (*ci)++;
        if (k < g_ret_n) { if (g_ret_n > 16) return NULL; return g_ret[g_ret_n - 1 - k]; }
        k -= g_ret_n; if (k < g_nrest) return (const uint8_t *)g_rest[k];
        return NULL; }
}
// Deferral pre-screen (binary search on the store lookahead index, zero scanning): for every color subresource of P, the first "used as attachment / cleared" event after this draw must be "used as attachment under a different SetRenderTargets,
// whose targets cover all of P's colors"; cleared first / never used as attachment again in this submit -> deferral is impossible, return directly. Read events carry no level; handed to the full lookahead to check by level.
static int defer_pre(void) {
    if (!g_sdx_ok || !g_exec_cur || !g_exec_seq || g_sdx_op[g_exec_seq - 1] != g_exec_cur) return 0;
    uint32_t P = g_exec_seq - 1;
    for (int k = 0; k < R.rt.n; k++) { if (!R.rt.color[k]) continue;
        uint64_t ser; int lv, sl; rt_sub(&R.rt, k, &ser, &lv, &sl);
        int b = sdx_bucket(ser, 0); if (b < 0) return 0;
        uint32_t lo = g_sdx_bst[b], end = g_sdx_bst[b + 1], hi = end;
        while (lo < hi) { uint32_t m = (lo + hi) / 2; if (g_sdx[m].seq <= P) lo = m + 1; else hi = m; }
        int ok = 0, same = 0; uint32_t walk = 0, prt = lo > g_sdx_bst[b] && g_sdx[lo - 1].seq == P ? g_sdx[lo - 1].rtseq : SDX_NONE;   // SetRenderTargets sequence number of P's own render pass (taken from this draw's used-as-attachment event)
        for (uint32_t j = lo; j < end; j++) { const sdx_ev *e = &g_sdx[j];
            if (++walk > 64) return 0;   // Too many events (a heavily sampled texture): not taken as a candidate, conservative and avoids a long walk
            if (g_hoist_n && hoist_skipped(e->op)) continue;
            if (e->kind == SDX_READ || e->kind == SDX_NEED) continue;   // Read events carry no level (reading another level is not a conflict); handed to the full lookahead to check by level
            if (e->plane != 0 || e->lv != lv || e->sl != sl) continue;
            if (e->kind == SDX_CLEAR) return 0;
            if (e->rtseq == SDX_NONE || e->rtseq >= g_sdx_nop) return 0;
            const uint8_t *rop = g_sdx_op[e->rtseq]; const p_set_rt *r = (const p_set_rt *)(rop + 8);
            if (rt_same(r, &R.rt)) { if (e->rtseq != prt) same = 1; continue; }   // Same target: one immediately following is a continuation segment of P, one separated by other render passes is Q; the pre-screen does not distinguish them, and if there is still no other candidate at the end it is handed to the full lookahead
            for (int i = 0; i < R.rt.n; i++) { if (!R.rt.color[i]) continue; int f = 0;
                for (int q = 0; q < r->n && !f; q++) if (r->color[q] && sub_eq(&R.rt, i, r, q)) f = 1;
                if (!f) return 0; }
            ok = 1; break; }
        if (!ok && !same) return 0; }
    return 1;
}
static void defer_try(const p_draw *dw, const p_draw_idx *di) {
    if (!defer_on() || defer_has_in_p() || g_defer_replaying || g_fz.active || g_fz_checking || !g_exec_next || !R.rt.n) return;
    int slot = defer_slot_free(); if (slot < 0) return;
    g_defer_cur = &g_defer_slots[slot];
    int ncol = 0; for (int i = 0; i < R.rt.n; i++) ncol += R.rt.color[i] != NULL; if (!ncol) return;   // Render passes with only depth-stencil attached (shadow maps etc.): Q is defined as covering P's colors, so a P without color can never be deferred -- previously the pre-screen let it through idly and the full lookahead scanned dozens of draws for nothing
    int has_clear = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) has_clear |= R.clear_c[i]; if (!has_clear) return;
    if (!defer_pre()) return;
    memset(&g_defer, 0, sizeof g_defer); g_defer.p_rt = R.rt; memcpy(g_defer.clear_c, R.clear_c, 8); memcpy(g_defer.cc, R.cc, sizeof R.cc);
    g_defer.clear_d = R.clear_d; g_defer.clear_s = R.clear_s; g_defer.cd = R.cd; g_defer.cs = R.cs;
    uint64_t tex[6][NTEX], img[6][32]; memcpy(tex, S.tex, sizeof tex); memcpy(img, S.img, sizeof img); const prog_t *pg = S.prog; ds_st ds = S.ds;
    const uint8_t *p = g_exec_next, *p_end = NULL, *qop = NULL; const p_set_rt *q = NULL; p_set_rt scan_rt = R.rt; int srt_hit = -1, ci = 0, draws = 1, p_has_draw = 1; defer_collect_reads(&g_defer, pg, tex, img);   // The current draw is P's first
    for (int guard = 0; guard < 200000; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        if (g_hoist_n && hoist_skipped(p)) { p += h.size; continue; }
        switch (h.op) {
            case OP_END: { const uint8_t *nx = defer_cont(&ci); if (!nx) { g_defer.reason = 1; goto fail; } p = nx; continue; }
            case OP_CALL: g_defer.reason = 2; goto fail;
            case OP_JUMP: memcpy(&p, a, 8); continue;
            case OP_BIND_PROGRAM: pg = ((const p_bind_program *)a)->prog; break;
            case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_IMG: { p_bind_handles one; const p_bind_handles *b = bind_rec(h.op, a, &one); bh_fresh(b); int lim = h.op == OP_BIND_IMG ? 32 : NTEX;
                if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) { if (h.op == OP_BIND_IMG) img[b->stage][b->first + k] = b->h[k]; else tex[b->stage][b->first + k] = b->h[k]; } break; }
            case OP_BIND_STATE: { const p_bind_state *b = a; if (b->kind == 5) memcpy(&ds, b->bytes, sizeof ds); break; }
            case OP_DRAW: case OP_DRAW_IDX:
                if (defer_prog_hits(pg, tex, img, &g_defer.p_rt, 1, 1)) { /* Suggestion: here, after identifying that an output mapping is needed but _bf is missing, call bf_want_note. */ g_defer.reason = 7; goto fail; }
                if (p_end) {   // Draws in the intermediate range: must not write textures P reads, must not write P's depth-stencil (and must not test stencil when P writes stencil)
                    if (srt_hit < 0) { srt_hit = 0; for (int k = 0; k < scan_rt.n && !srt_hit; k++) if (scan_rt.color[k] && defer_rt_hits_reads(&scan_rt, k)) srt_hit = 1; }   // Attachments change only at SetRenderTargets: computed once per segment
                    if (srt_hit) { g_defer.reason = 12; goto fail; }
                    if (scan_rt.depth && g_defer.p_rt.depth && scan_rt.depth->serial == g_defer.p_rt.depth->serial && (ds.dw || (g_defer.p_st && ds.st))) { g_defer.reason = 13; goto fail; }
                    break; }
                draws++; if (draws > 64) { g_defer.reason = 20; goto fail; } p_has_draw = 1; defer_collect_reads(&g_defer, pg, tex, img); g_defer.p_dw |= ds.dw; g_defer.p_st |= ds.st && ((S.st_wmask[0] | S.st_wmask[1]) != 0) && (ds.fail_f != NVN_STENCIL_OP_KEEP || ds.zfail_f != NVN_STENCIL_OP_KEEP || ds.zpass_f != NVN_STENCIL_OP_KEEP || ds.fail_b != NVN_STENCIL_OP_KEEP || ds.zfail_b != NVN_STENCIL_OP_KEEP || ds.zpass_b != NVN_STENCIL_OP_KEEP); if (g_defer.p_dw) { g_defer.reason = 8; goto fail; } break;
            case OP_RESET_COUNTER: g_defer.reset_between = 1; break;
            case OP_COPY_T2T: { const p_copy_t2t *c = a;   // Texture copy: allowed only if neither end touches P's attachments and the destination is not a texture P samples
                uint64_t ss = c->src ? c->src->serial : 0, ds_ = c->dst ? c->dst->serial : 0; int bad = !ss || !ds_;
                for (int k = 0; k < g_defer.p_rt.n && !bad; k++) if (g_defer.p_rt.color[k] && (g_defer.p_rt.color[k]->serial == ss || g_defer.p_rt.color[k]->serial == ds_)) bad = 1;
                if (g_defer.p_rt.depth && (g_defer.p_rt.depth->serial == ss || g_defer.p_rt.depth->serial == ds_)) bad = 1;
                for (int k = 0; k < g_defer.nreads && !bad; k++) if (h_ser(g_defer.p_reads[k]) == ds_) bad = 1;
                if (g_defer.nreads > 512) bad = 1;
                if (bad) { g_defer.reason = 3; g_defer.reason_op = h.op; goto fail; } break; }
            case OP_REPORT_COUNTER: case OP_RENDER_COND: case OP_UPDATE_UB: case OP_COPY_B2B: case OP_CLEAR_BUF: case OP_COPY_B2T: case OP_COPY_T2B: case OP_CLEAR_TEX: case OP_DISPATCH: g_defer.reason = 3; g_defer.reason_op = h.op; goto fail;
            case OP_CLEAR_COLOR: { const p_clear_color *c = a; if (!p_end) { g_defer.reason = 16; goto fail; }   // A clear inside P's continuation segment: do not defer
                if (c->index >= 0 && c->index < scan_rt.n && (defer_rt_hits_reads(&scan_rt, c->index) || (scan_rt.color[c->index] && defer_rt_hits_reads(&g_defer.p_rt, c->index)))) { g_defer.reason = 5; goto fail; } break; }
            case OP_CLEAR_DS: if (!p_end) { g_defer.reason = 16; goto fail; } break;
            case OP_SET_RT: { const p_set_rt *r = a;
                if (!p_end && rt_same(r, &g_defer.p_rt)) break;   // The same target set once more (the game often sends it repeatedly): still a continuation segment of P, and its draws are deferred together
                if (!p_end) p_end = p; int shared = 0; for (int i = 0; i < g_defer.p_rt.n; i++) for (int j = 0; j < r->n; j++) if (g_defer.p_rt.color[i] && r->color[j] && sub_eq(&g_defer.p_rt, i, r, j)) shared = 1;
                if (shared) {   // An empty render pass (targets set, then changed again without drawing or clearing) touches no attachment: it is neither taken as Q nor counted as a conflict
                    const uint8_t *e = p + h.size; int ec = ci, empty = 0;
                    for (int g2 = 0; g2 < 200000; g2++) { op_hdr he; memcpy(&he, e, sizeof he);
                        if (he.op == OP_JUMP) { memcpy(&e, e + 8, 8); continue; }
                        if (he.op == OP_END) { const uint8_t *nx = defer_cont(&ec); if (!nx) break; e = nx; continue; }
                        if (he.op == OP_SET_RT) { empty = 1; break; }
                        if (he.op == OP_DRAW || he.op == OP_DRAW_IDX || he.op == OP_CLEAR_COLOR || he.op == OP_CLEAR_DS || he.op == OP_CALL || he.op == OP_DISPATCH || he.op == OP_CLEAR_TEX || he.op == OP_COPY_T2T || he.op == OP_COPY_B2T || he.op == OP_COPY_T2B) break;
                        e += he.size; }
                    if (empty) break; }
                if (!shared) break; if (!p_has_draw || !defer_q_superset(r)) { g_defer.reason = 6; goto fail; } q = r; qop = p; goto q_found; }
            default: break;
        }
        if (h.op == OP_SET_RT) { scan_rt = *(const p_set_rt *)a; srt_hit = -1; }
        p += h.size;
    }
fail:
    if (g_defer.reason && passdump()) PD("G %llu 不成立 reason=%d op=%d", g_stat_pass + 1, g_defer.reason, g_defer.reason_op); memset(&g_defer, 0, sizeof g_defer); return;
q_found:
    p = qop; op_hdr qh; memcpy(&qh, p, sizeof qh); p += qh.size; int q_body = 0, q_clears = 0;   // Binding tracking continues from the main scan's state (does not go back to S at the start of P)
    for (int guard = 0; guard < 200000; guard++) { op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8; if (h.op == OP_JUMP) { memcpy(&p, a, 8); continue; }
        // The whole Q segment (up to the next SetRenderTargets / end of list): no draw may sample P's color attachments through a view covering P's level -- after deferral P's result is still in the tile and has not landed in main memory, so sampling would read old content
        if (h.op == OP_DRAW || h.op == OP_DRAW_IDX) { if (defer_prog_hits(pg, tex, img, &g_defer.p_rt, 1, 1)) { g_defer.reason = 14; goto fail; } q_body = 1; p += h.size; continue; }
        if (q_body && (h.op == OP_SET_RT || h.op == OP_END)) break;
        if (!q_body && h.op == OP_END) { const uint8_t *nx = defer_cont(&ci); if (!nx) { g_defer.reason = 9; goto fail; } p = nx; continue; }
        if (h.op == OP_CALL) { g_defer.reason = 9; g_defer.reason_op = h.op; goto fail; }
        // After Q's SetRenderTargets and before its first draw and any clear, another SetRenderTargets arrives (the game sets targets repeatedly, the previous one being an empty render pass): if the new targets still cover P take it as Q, otherwise do not defer
        if (!q_body && h.op == OP_SET_RT && !q_clears) { const p_set_rt *r2 = a; if (!defer_q_superset(r2)) { g_defer.reason = 15; goto fail; } q = r2; qop = p; p += h.size; continue; }
        if (h.op == OP_CLEAR_COLOR || h.op == OP_CLEAR_DS) q_clears = 1;
        if (!q_body && (h.op == OP_END || h.op == OP_SET_RT || h.op == OP_UPDATE_UB || h.op == OP_COPY_B2B || h.op == OP_CLEAR_BUF || h.op == OP_DISPATCH)) { g_defer.reason = 9; g_defer.reason_op = h.op; goto fail; }
        if (h.op == OP_BIND_PROGRAM) pg = ((const p_bind_program *)a)->prog; else if (h.op == OP_BIND_TEX || h.op == OP_BIND_TEX1 || h.op == OP_BIND_IMG) { p_bind_handles one; const p_bind_handles *b = bind_rec(h.op, a, &one); bh_fresh(b); int lim = h.op == OP_BIND_IMG ? 32 : NTEX; if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) { if (h.op == OP_BIND_IMG) img[b->stage][b->first + k] = b->h[k]; else tex[b->stage][b->first + k] = b->h[k]; } }
        else if (h.op == OP_CLEAR_COLOR) { const p_clear_color *c = a; if (c->index >= 0 && c->index < q->n && q->color[c->index]) for (int i = 0; i < g_defer.p_rt.n; i++) if (g_defer.p_rt.color[i] && sub_eq(q, c->index, &g_defer.p_rt, i)) { g_defer.reason = 10; goto fail; } }
        p += h.size;
    }
    if (!q || !defer_q_superset(q) || g_defer.n >= 64) { g_defer.reason = 11; goto fail; }
    for (int i = 0; i < 4; i++) if (&g_defer_slots[i] != &g_defer && g_defer_slots[i].active && g_defer_slots[i].q_op == qop && !defer_same_q_depth(&g_defer_slots[i], q)) { g_defer.reason = 17; goto fail; }
    g_defer.q_rt = *q; if (!q->depth && g_defer.p_rt.depth) { g_defer.q_rt.depth = g_defer.p_rt.depth; g_defer.q_rt.dview = g_defer.p_rt.dview; g_defer.q_rt.has_dview = g_defer.p_rt.has_dview; } { int na = 0; for (int i = 0; i < 4; i++) { const defer_t *d = &g_defer_slots[i]; if (d != &g_defer && d->active && d->at_q) na += d->n; }
      if (na + 1 > 64) { g_defer.reason = 20; goto fail; } }
    g_defer.q_op = qop; g_defer.p_end_op = p_end; g_defer.in_p = 1; g_defer.active = 1; g_defer_nact++; g_defer_pend = p_end; g_defer_n++; g_defer.born = g_defer_n;
    // Chained: this render pass is exactly the Q of upstream groups (they are at_q, waiting to be replayed when this render pass opens), and this render pass is itself deferred as a whole -- the upstream groups are merged into the new group in creation order:
    // the target space of an upstream group = this render pass's targets = the new group's P targets, and the snapshot's mapping is composed as pm' = upstream lmap o pm; the upstream clears are already in R and are carried along with the new group below
    for (int gi, after = 0; (gi = defer_next(1, after)) >= 0;) { defer_t *d = &g_defer_slots[gi]; after = d->born; if (d == &g_defer) continue;
        for (int k = 0; k < 8; k++) if (d->lmap[k] != 0xff && d->lmap[k] != k) { g_defer.reason = 18; goto fail; }   // Only merge in upstream groups with an identity mapping (see defer_q_superset)
        for (int i = 0; i < d->n; i++) { defer_snap z = d->snap[i]; uint8_t pm[8]; for (int k = 0; k < 8; k++) pm[k] = d->lmap[k] == 0xff || d->lmap[k] >= 8 ? 0xff : z.pm[d->lmap[k]];
            memcpy(z.pm, pm, 8); g_defer.snap[g_defer.n++] = z; }
        g_defer.reset_between |= d->reset_between; d->n = 0; defer_release_one(d); } if (!defer_take(dw, di)) { defer_release_one(&g_defer); return; }
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0; PD("G %llu 成立 组 %d P→Q draws=%d", g_stat_pass + 1, slot, draws);
}
// Pre-screen of the necessary condition for merging (binary search on the submit index, zero scanning on failure): merging requires that "the first draw of the later pass (the next SET_RT with a different target that has not been hoisted)
// samples some color attachment of the earlier pass", i.e. there is an SDX_READ event for that attachment at that draw's sequence number. If not satisfied there is certainly no merge, saving the per-operation scan below;
// only if satisfied does it go through the original planning (the decision is exactly the same as without the pre-screen). When the index is unavailable, plan as before.
static int fz_prefilter(void) {
    if (!g_sdx_ok || !g_exec_seq || g_sdx_op[g_exec_seq - 1] != g_exec_cur) return 1;
    uint32_t P = g_exec_seq - 1, lo = 0, hi = g_rtx_n;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (g_rtx_seq[m] <= P) lo = m + 1; else hi = m; }
    for (; lo < g_rtx_n; lo++) { const uint8_t *op = g_sdx_op[g_rtx_seq[lo]]; if (g_hoist_n && hoist_skipped(op)) continue; if (!rt_same(&R.rt, (const p_set_rt *)(op + 8))) break; }
    if (lo >= g_rtx_n) return 0;
    uint32_t r2 = g_rtx_seq[lo], a = 0, b = g_drx_n;
    while (a < b) { uint32_t m = (a + b) / 2; if (g_drx_seq[m] <= r2) a = m + 1; else b = m; }
    if (a >= g_drx_n) return 0;
    uint32_t fd = g_drx_seq[a];
    for (uint32_t k = lo + 1; k < g_rtx_n && g_rtx_seq[k] < fd; k++) if (!(g_hoist_n && hoist_skipped(g_sdx_op[g_rtx_seq[k]]))) return 0;   // The target changes again before the first draw: step two of the original planning gives up as well
    for (int j = 0; j < R.rt.n && j < 8; j++) { if (!R.rt.color[j]) continue; int bk = sdx_bucket(R.rt.color[j]->serial, 0); if (bk < 0) continue;
        uint32_t e0 = g_sdx_bst[bk], e1 = g_sdx_bst[bk + 1];
        while (e0 < e1) { uint32_t m = (e0 + e1) / 2; if (g_sdx[m].seq < fd) e0 = m + 1; else e1 = m; }
        for (; e0 < g_sdx_bst[bk + 1] && g_sdx[e0].seq == fd; e0++) if (g_sdx[e0].kind == SDX_READ) return 1; }
    return 0;
}
static void fuse_try(void) {
    if (g_fz.active || !g_exec_next || g_hoist_n + 12 > HOIST_MAX || !R.rt.n) return;
    if (!fz_prefilter()) return;
    // One: from the earlier pass to the next SetRenderTargets, only draws and state are allowed
    const uint8_t *p = NULL, *r2op = NULL, *reuse[8]; int nreuse = 0; const p_set_rt *r2 = NULL; p_scissor sc = R.sc; int has_sc = R.has_sc;
    fz_cursor cur = {.p = g_exec_next}; op_hdr h; const void *a;
    for (int guard = 0; guard < 200000 && !r2; guard++) {
        if (!fz_next(&cur, &p, &h, &a)) return;
        switch (h.op) {
            case OP_COPY_T2T: case OP_COPY_B2T: case OP_COPY_T2B: case OP_CLEAR_TEX: case OP_DISPATCH: case OP_RENDER_COND:
            case OP_CLEAR_COLOR: case OP_CLEAR_DS: case OP_UPDATE_UB: case OP_COPY_B2B: case OP_CLEAR_BUF: return;
            case OP_SCISSOR: sc = *(const p_scissor *)a; has_sc = 1; break;
            case OP_SET_RT: {
                int pend = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
                if (rt_same(&R.rt, a)) {
                    if (g_enc_mode == 1 || pend || !R.has_rt || !tbdr_on() || nreuse >= 8) return;
                    reuse[nreuse++] = p; continue;
                }
                r2 = a; r2op = p; continue;
            }
            default: break;
        }
    }
    if (!r2 || rt_same(&R.rt, r2)) return;
    // Two: from the start of the later pass to its first draw, only clears (not clearing shared attachments) and state are allowed
    const uint8_t *cl[8]; int ncl = 0; uint8_t clr[8] = {0}; MTLClearColor cc[8]; int cd_f = 0, cs_f = 0; double cdv = 0; uint32_t csv = 0;
    if (!fz_next(&cur, &p, &h, &a)) return;   // Continue from the cursor of step one (already past the later pass's SET_RT, keeping the cross-handle / return stack position)
    for (int guard = 0; guard < 200000; guard++) {
        if (h.op == OP_DRAW || h.op == OP_DRAW_IDX) break;
        switch (h.op) {
            case OP_COPY_T2T: case OP_COPY_B2T: case OP_COPY_T2B: case OP_CLEAR_TEX: case OP_DISPATCH: case OP_SET_RT: case OP_UPDATE_UB: case OP_COPY_B2B: case OP_CLEAR_BUF: case OP_RENDER_COND: return;
            case OP_SCISSOR: sc = *(const p_scissor *)a; has_sc = 1; break;
            case OP_CLEAR_COLOR: { const p_clear_color *c = a; if (ncl >= 8 || c->index < 0 || c->index >= r2->n || !r2->color[c->index] || c->mask != 0xf) return;
                const tex_t *t = r2->color[c->index];
                if (has_sc && !(sc.x <= 0 && sc.y <= 0 && sc.x + sc.w >= (int)t->d.width && sc.y + sc.h >= (int)t->d.height)) return;
                clr[c->index] = 1; cc[c->index] = MTLClearColorMake(c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]); cl[ncl++] = p; break; }
            case OP_CLEAR_DS: { const p_clear_ds *c = a; if (ncl >= 8 || !r2->depth) return;
                if (c->depth_mask) { cd_f = 1; cdv = c->depth; } if (c->stencil_mask) { cs_f = 1; csv = (uint32_t)c->stencil; } cl[ncl++] = p; break; }
            default: break;
        }
        if (!fz_next(&cur, &p, &h, &a)) return;
    }
    // Three: union of attachments (the earlier pass's attachments keep their original indices, those unique to the later pass are appended), consistent sizes, compatible depth
    p_set_rt U = R.rt; int n = R.rt.n; int8_t p2map[8]; memset(p2map, -1, sizeof p2map);
    for (int k = 0; k < r2->n && k < 8; k++) { if (!r2->color[k]) continue;
        int j = -1; for (int m = 0; m < R.rt.n; m++) if (R.rt.color[m] && sub_eq(&R.rt, m, r2, k)) { j = m; break; }
        if (j < 0) { if (n >= 8) return; j = n++; U.color[j] = r2->color[k]; U.cview[j] = r2->cview[k]; U.has_cview[j] = r2->has_cview[k]; }
        else if (clr[k]) return;   // The start of the later pass wants to clear an attachment the earlier pass is drawing to: after merging this clear has nowhere to go
        p2map[k] = (int8_t)j; }
    U.n = n;
    { uint64_t s0; int l0, x0; rt_sub(&U, 0, &s0, &l0, &x0); if (!U.color[0]) return;
      uint32_t w0 = MAX(U.color[0]->d.width >> l0, 1u), h0 = MAX(U.color[0]->d.height >> l0, 1u);
      for (int j = 1; j < n; j++) { uint64_t s_; int l_, x_; rt_sub(&U, j, &s_, &l_, &x_); if (!U.color[j] || MAX(U.color[j]->d.width >> l_, 1u) != w0 || MAX(U.color[j]->d.height >> l_, 1u) != h0) return; } }
    int depth_from_r2 = 0;
    if (R.rt.depth && r2->depth && !sub_eq(&R.rt, 8, r2, 8)) return;
    if (!R.rt.depth && r2->depth) { U.depth = r2->depth; U.dview = r2->dview; U.has_dview = r2->has_dview; depth_from_r2 = 1; }
    if ((cd_f || cs_f) && !depth_from_r2) return;   // The depth is already in use in the earlier pass: the depth clear at the start of the later pass has nowhere to go
    // Four: draws of the earlier pass must not sample attachments brought in by the later pass; when the depth comes from the later pass, depth-stencil testing must not be enabled
    static uint64_t tex[6][NTEX], img[6][32]; memcpy(tex, S.tex, sizeof tex); memcpy(img, S.img, sizeof img);
    const prog_t *pg = S.prog; ds_st ds = S.ds;
    cur = (fz_cursor){.p = g_exec_next};
    for (;;) {   // The first loop iteration checks the draw that opens the render pass (current state), then checks the draws in the range one by one
        if (depth_from_r2 && (ds.dt || ds.dw || ds.st)) return;
        for (int j = R.rt.n; j < n; j++) if (prog_samples_ser(pg, tex, img, U.color[j]->serial)) return;
        if (depth_from_r2 && prog_samples_ser(pg, tex, img, U.depth->serial)) return;
        for (;;) {
            const uint8_t *q; op_hdr hq; const void *aq;
            if (!fz_next(&cur, &q, &hq, &aq)) return;
            if (q == r2op) goto planned;
            if (hq.op == OP_BIND_PROGRAM) pg = ((const p_bind_program *)aq)->prog;
            else if (hq.op == OP_BIND_STATE) { const p_bind_state *b = aq; if (b->kind == 5) memcpy(&ds, b->bytes, sizeof ds); }
            else if (hq.op == OP_BIND_TEX || hq.op == OP_BIND_TEX1 || hq.op == OP_BIND_IMG) { p_bind_handles one; const p_bind_handles *b = bind_rec(hq.op, aq, &one); bh_fresh(b); int lim = hq.op == OP_BIND_IMG ? 32 : NTEX;
                if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) { if (hq.op == OP_BIND_IMG) img[b->stage][b->first + k] = b->h[k]; else tex[b->stage][b->first + k] = b->h[k]; } }
            else if (hq.op == OP_DRAW || hq.op == OP_DRAW_IDX) break;
        }
    }
planned:
    {   // Five: pre-screen the first draw of the later pass (static conditions), to avoid attaching the union of attachments for nothing: track the program and texture bindings up to that draw
        const prog_t *pg2 = S.prog; static uint64_t t2[6][NTEX], i2[6][32]; memcpy(t2, S.tex, sizeof t2); memcpy(i2, S.img, sizeof i2);
        cur = (fz_cursor){.p = g_exec_next};
        for (;;) { const uint8_t *q; op_hdr hq; const void *aq; if (!fz_next(&cur, &q, &hq, &aq)) return; if (q == p) break;
            if (hq.op == OP_BIND_PROGRAM) pg2 = ((const p_bind_program *)aq)->prog;
            else if (hq.op == OP_BIND_TEX || hq.op == OP_BIND_TEX1 || hq.op == OP_BIND_IMG) { p_bind_handles one; const p_bind_handles *b = bind_rec(hq.op, aq, &one); bh_fresh(b); int lim = hq.op == OP_BIND_IMG ? 32 : NTEX;
                if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) { if (hq.op == OP_BIND_IMG) i2[b->stage][b->first + k] = b->h[k]; else t2[b->stage][b->first + k] = b->h[k]; } }
        }
        NvProg *pr2 = pg2 ? prog_get(pg2) : nil; NvShader *vs2 = pr2 ? pr2->sh[0] : nil, *fs2 = pr2 ? pr2->sh[1] : nil;
        if (!vs2 || !fs2) return;
        int touches = 0;
        for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr2->sh[st]; if (!sh || sh->stage >= 6) continue;
            for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *bb = &sh->rf.b[i]; uint64_t h;
                if (bb->kind == NVN_RB_IMG) h = bb->reg < 32 ? i2[sh->stage][bb->reg] : 0; else if (bb->kind == NVN_RB_TEX || bb->kind == NVN_RB_TBUF) h = bb->reg < NTEX ? t2[sh->stage][bb->reg] : 0; else continue;
                uint64_t ser = h ? h_ser(h) : 0; if (!ser) continue;
                int j = -1; for (int m = 0; m < R.rt.n; m++) if (R.rt.color[m] && R.rt.color[m]->serial == ser) { j = m; break; }
                if (j < 0) continue;
                if (sh != fs2 || bb->kind != NVN_RB_TEX || (fs2->rf.fc_mixed[bb->reg >> 6] >> (bb->reg & 63) & 1)) return;
                int any = 0;
                for (int c = 0; c < fs2->rf.nfc; c++) if (fs2->rf.fc_reg[c] == bb->reg) { int l = fs2->rf.fc_locn[c];
                    if (l >= 32 || (vs2->rf.uv_attr[l] < 0 && vs2->rf.uv_aff_attr[l] < 0)) return; any = 1; }
                if (!any) return;
                touches = 1; } }
        if (!touches) return;   // The later pass does not read the earlier pass's attachments: merging has no benefit
        int ident = 1; for (int c = 0; c < 8; c++) if (fs2->rf.color_mask >> c & 1) { if (p2map[c] < 0) return; if (p2map[c] != c) ident = 0; }
        if (!ident && !fs2->rf.nfo) return;
    }
    memset(&g_fz, 0, sizeof g_fz);
    g_fz.active = 1; g_fz.phase = 1; g_fz.n1 = R.rt.n; g_fz.nreuse = nreuse; g_fz.rt1 = R.rt; g_fz.rt2 = *r2; g_fz.rt2_op = r2op; memcpy(g_fz.reuse, reuse, sizeof reuse); memcpy(g_fz.p2map, p2map, 8);
    for (int j = 0; j < 8; j++) g_fz.lmap1[j] = j < R.rt.n ? (uint8_t)j : 0xff;
    fz_probe_cov2(r2, p, p2map, clr, g_fz.n1, n);
    g_fz.skip[g_fz.nskip++] = r2op; for (int i = 0; i < ncl; i++) g_fz.skip[g_fz.nskip++] = cl[i];
    for (int i = 0; i < g_fz.nskip; i++) g_hoist_skip[g_hoist_n++] = g_fz.skip[i];
    R.rt = U;
    memcpy(g_fz.clr2, clr, 8); memcpy(g_fz.cc2, cc, sizeof cc); g_fz.cd2f = (uint8_t)cd_f; g_fz.cs2f = (uint8_t)cs_f; g_fz.cd2 = cdv; g_fz.cs2 = csv;
    for (int k = 0; k < 8; k++) if (clr[k]) { R.clear_c[p2map[k]] = 1; R.cc[p2map[k]] = cc[k]; }
    if (cd_f) { R.clear_d = 1; R.cd = cdv; } if (cs_f) { R.clear_s = 1; R.cs = csv; }
    if (!g_fz_n++) MLOG("可读 tile 通道合并：首例 通道 %llu（物理附件 %d 个）", g_stat_pass + 1, n);
    PD("F %llu 合并下一个通道（可读 tile）：物理附件 %d 个，前一个通道 %d 个", g_stat_pass + 1, n, R.rt.n == n ? g_fz.n1 : g_fz.n1);
}
static id<MTLFunction> bf_fn(NvShader *fs, const int *fx, uint8_t fbmask, const uint8_t *lmap) {   // The readable tile version is specialized by constants (cached)
    os_unfair_lock_lock(&g_fz_fn_lock);
    if (!g_bf_fn_cache) { g_bf_fn_cache = [NSMutableDictionary dictionary]; g_bf_lib_cache = [NSMutableDictionary dictionary]; }
    size_t kl = strlen(fs->key); NSString *base = [NSString stringWithFormat:@"%.*s", (int)(kl > 2 ? kl - 2 : kl), fs->key];
    id<MTLLibrary> lib = g_bf_lib_cache[base];
    if (!lib) {
        NSError *e = nil; NSString *path = [NSString stringWithFormat:@"%@/%@_bf.metallib", g_msl_dir, base];
        lib = lib_from_file(path, &e);
        if (!lib) MLOG("【红】可读 tile 版本载入失败 %s：%s", path.UTF8String, e.localizedDescription.UTF8String);
        else g_bf_lib_cache[base] = lib;
    }
    if (!lib) { os_unfair_lock_unlock(&g_fz_fn_lock); return nil; }
    NSMutableString *k = [NSMutableString stringWithFormat:@"%02x", fbmask];
    for (int i = 0; i < fs->rf.nfc; i++) [k appendFormat:@",%d", fx[i]];
    for (int j = 0; j < 8; j++) [k appendFormat:@"/%02x", lmap[j]];
    NSString *cacheKey = [NSString stringWithFormat:@"%@|%@", base, k];
    id<MTLFunction> f = g_bf_fn_cache[cacheKey];
    if (f) { os_unfair_lock_unlock(&g_fz_fn_lock); return f; }
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
    for (int i = 0; i < fs->rf.nfc; i++) { int v = fx[i]; [cv setConstantValue:&v type:MTLDataTypeInt atIndex:(NSUInteger)i]; }
    for (int q = 0; q < 8; q++) { bool b = fbmask >> q & 1; [cv setConstantValue:&b type:MTLDataTypeBool atIndex:(NSUInteger)(32 + q)]; }
    for (int j = 0; j < fs->rf.nfo; j++) for (int q = 0; q < 8; q++) { bool b = lmap[q] == fs->rf.fo_color[j]; [cv setConstantValue:&b type:MTLDataTypeBool atIndex:(NSUInteger)(64 + 8 * j + q)]; }
    NSError *e = nil; f = [lib newFunctionWithName:@"main0" constantValues:cv error:&e];
    if (!f) { MLOG("【红】可读 tile 版本特化失败 %s（%s）：%s", fs->key, k.UTF8String, e.localizedDescription.UTF8String); os_unfair_lock_unlock(&g_fz_fn_lock); return nil; }
    g_bf_fn_cache[cacheKey] = f; os_unfair_lock_unlock(&g_fz_fn_lock); return f;
}
static int ubo_float(int slot, uint16_t ref, float *out) {   // Component cmp of vector vec of the uniform slot (read on the CPU side; not read if it falls within a buffer write pending execution in this submit)
    int sl = ref >> 8, vec = ref >> 2 & 63, cmp = ref & 3;
    if (slot < 0 || slot >= 6 || sl < 0 || sl >= 16) return 0;
    const p_bind_buf *u = &S.ubo[slot][sl]; uint64_t off = (uint64_t)vec * 16 + (uint64_t)cmp * 4;
    if (!u->addr || off + 4 > u->size || pend_hits(u->addr + off, 4)) return 0;
    void *mb; uint64_t mo; const uint8_t *base;
    if (!mtl_addr(u->addr + off, 4, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) return 0;
    memcpy(out, base + mo, 4); return 1;
}
static int zskip_fact_for(const char *key, zskip_fact *out) {
    if (!key) return 0;
    for (int i = 0; i < g_zskip_nf; i++) if (!strncmp(key, g_zskip_facts[i].key, strlen(g_zskip_facts[i].key))) { *out = g_zskip_facts[i]; return 1; }   // Runtime keys carry the _b suffix
    return 0;
}
static int zskip_view_zero(int tid) {
    if (!tid || tid >= g_ntreg || !g_treg[tid].ok) return 0;
    treg_t *r = &g_treg[tid]; const tex_t *t = &r->t; int lv0 = r->has_v && r->v.set_levels ? r->v.base_level : 0;
    int nl = r->has_v && r->v.set_levels && r->v.nlevels > 0 ? r->v.nlevels : 1;
    int sl0 = r->has_v && r->v.set_layers ? r->v.min_layer : 0;
    int ns = r->has_v && r->v.set_layers && r->v.nlayers > 0 ? r->v.nlayers : 1;
    for (int l = 0; l < nl; l++) for (int s = 0; s < ns; s++) if (!zskip_has(t->serial, lv0 + l, sl0 + s)) return 0;
    return 1;
}
// While an occlusion query is counting, a skipped draw should have contributed passed samples: it only does not matter if "there is no report before the next reset".
// Looking forward in replay order from after this draw: sample count reset encountered first -> does not matter (return 1); sample count report encountered first, or the end cannot be seen (submit ends) -> matters (return 0)
static int zskip_vis_dead(void) {
    if (!g_vis_counting) return 1;
    const uint8_t *p = g_exec_next; int ci = 0; if (!p) return 0;
    for (int guard = 0; guard < 200000; guard++) { op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        if (h.op == OP_JUMP) { memcpy(&p, a, 8); continue; }
        if (h.op == OP_END) { p = defer_cont(&ci); if (!p) return 0; continue; }
        if (h.op == OP_CALL) return 0;
        if (h.op == OP_RESET_COUNTER || h.op == OP_REPORT_COUNTER) { uint64_t t; memcpy(&t, a, 8); if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) return h.op == OP_RESET_COUNTER; }
        p += h.size; }
    return 0;
}
static int zskip_can_draw(NvProg *pr) {
    // Skip conditions (all must hold): the fragment key is in the fact table, the view bound to every texture slot declared by its reflection covers only subresources known to be all zero, the coefficient is finite,
    // no depth write and no stencil, not counting an occlusion query, and every color attachment to be written is already 0 (a candidate cleared entirely to 0 in this render pass, or known all zero) -- writing 0 over 0.
    // Reasons for not holding (the N line of PASSDUMP): 1 depth / stencil 2 occlusion query counting with a report before the reset 3 coefficient unreadable or non-finite 4 image slot 5 sampled view not known all zero 6 no target 7 target is not 0
    if (!zskip_on() || !pr) return 0;
    NvShader *fs = nil; for (int st = 0; st < 5; st++) if (pr->sh[st] && pr->sh[st]->stage == 1) fs = pr->sh[st];
    zskip_fact f; float coeff; int why = 0;
    if (!fs || !zskip_fact_for(fs->key, &f)) return 0;
    if (S.ds.dw || S.ds.st) why = 1;
    else if (!zskip_vis_dead()) why = 2;
    else if (f.ubo_slot >= 0 && (!ubo_float(1, (uint16_t)((f.ubo_slot << 8) | (f.vec << 2) | f.comp), &coeff) || !isfinite(coeff))) why = 3;   // ubo_slot < 0: this shader has no coefficient
    for (int i = 0; !why && i < fs->rf.nb; i++) { nvn_rbind *b = &fs->rf.b[i];
        if (b->kind == NVN_RB_IMG) { why = 4; break; }
        if (b->kind != NVN_RB_TEX && b->kind != NVN_RB_TBUF) continue;
        int tid = b->reg < NTEX ? (int)(S.tex[1][b->reg] & 0xfffff) : 0; if (!zskip_view_zero(tid)) why = 5; }
    if (!why && (!R.has_rt || !R.rt.n)) why = 6;
    for (int i = 0; !why && i < R.rt.n && i < 8; i++) { if (!R.rt.color[i]) continue;
        int pending = g_enc ? g_zskip_cand_ok[i] : zskip_zero_clear(i);   // Render pass already open: look at the candidates noted when the render pass was opened (not yet drawn to); not open: look at the pending clears
        if (!pending && (g_enc || R.clear_c[i] || !zskip_target_zero(R.rt.color[i], &R.rt.cview[i], R.rt.has_cview[i]))) why = 7; }
    if (why) PD("N %llu 零源不跳过 reason=%d 集合 %d", g_stat_pass + 1, why, g_zskip_n);
    return !why;
}
static int uv_proof(NvShader *vs, int locn, const p_draw *dw, const p_draw_idx *di) {   // The interpolated output locn at every pixel center is exactly this pixel's texel center: u = (x + 1) / 2, v = (1 - y) / 2, w = 1, viewport and scissor cover the whole block
    if (locn < 0 || locn >= 32 || vs->rf.pos_attr < 0) return 0;
    int direct = vs->rf.uv_attr[locn] >= 0, aff = vs->rf.uv_aff_attr[locn] >= 0;
    if (!direct && !aff) return 0;
    float sc[4] = {1, 0, 1, 0};   // Pass-through as-is = scale 1, offset 0
    if (!direct) for (int q = 0; q < 4; q++) if (!ubo_float(0, vs->rf.uv_aff[locn][q], &sc[q])) return 0;
    NSUInteger W = g_pass.tw, H = g_pass.th;
    if (S.has_vp && !(S.vp[0][0] == 0 && S.vp[0][1] == 0 && S.vp[0][2] == (float)W && S.vp[0][3] == (float)H)) return 0;
    if (R.has_sc && !(R.sc.x <= 0 && R.sc.y <= 0 && R.sc.x + R.sc.w >= (int)W && R.sc.y + R.sc.h >= (int)H)) return 0;
    float P[64][4], Q[64][4]; int n = dw ? dw->count : di->count;
    if (attr_read(vs->rf.pos_attr, dw, di, P) || attr_read(direct ? vs->rf.uv_attr[locn] : vs->rf.uv_aff_attr[locn], dw, di, Q)) return 0;
    for (int k = 0; k < n; k++) { float w = vs->rf.pos_w1 ? 1.0f : P[k][3];
        float u = direct ? Q[k][0] : fmaf(Q[k][0], sc[0], sc[1]), v = direct ? Q[k][1] : fmaf(Q[k][1], sc[2], sc[3]);   // The same fma as the shader
        if (w != 1.0f || u != (P[k][0] + 1.0f) * 0.5f || v != (1.0f - P[k][1]) * 0.5f) { PD("F %llu uv 证明：顶点 %d 位置 %g,%g uv %g,%g（缩放 / 偏移 %g,%g,%g,%g）", g_stat_pass, k, P[k][0], P[k][1], u, v, sc[0], sc[1], sc[2], sc[3]); return 0; } }
    return 1;
}
// One draw of the later pass: verify and produce the specialization. Returns 0 = cannot be proven (the caller ends the merged render pass and reopens with the original targets)
static int fz_p2_draw(NvProg *pr, const p_draw *dw, const p_draw_idx *di, uint8_t lmap[8], __strong id<MTLFunction> *fn) {
    NvShader *vs = pr->sh[0], *fs = pr->sh[1]; *fn = nil;
    if (!vs || !fs) { PD("F %llu 证不出：无顶点 / 片元着色器", g_stat_pass); return 0; }
    if (!g_fz.rt2.depth && R.rt.depth && (S.ds.dt || S.ds.dw || S.ds.st)) { PD("F %llu 证不出：深度来自前一个通道而本次绘制开了深度模板测试", g_stat_pass); return 0; }
    int fx[32]; for (int i = 0; i < 32; i++) fx[i] = -1; uint8_t fbmask = 0;
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || sh->stage >= 6) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 ? S.img[sh->stage][b->reg] : 0; else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = b->reg < NTEX ? S.tex[sh->stage][b->reg] : 0; else continue;
            uint64_t ser = h ? h_ser(h) : 0; if (!ser) continue;
            int j = -1; for (int m = 0; m < g_fz.n1; m++) if (R.rt.color[m] && R.rt.color[m]->serial == ser) { j = m; break; }
            if (j < 0) continue;   // What is sampled is not an attachment of the earlier pass
            if (sh != fs || b->kind != NVN_RB_TEX) { PD("F %llu 证不出：非片元阶段或非贴图槽读前一个通道的附件", g_stat_pass); return 0; }
            if (fs->rf.fc_mixed[b->reg >> 6] >> (b->reg & 63) & 1) { PD("F %llu 证不出：槽 %d 混用", g_stat_pass, b->reg); return 0; }
            uint64_t s_; int lv, sl; rt_sub(&R.rt, j, &s_, &lv, &sl);
            if (lv || sl || MAX(R.rt.color[j]->d.width, 1u) != g_pass.tw || MAX(R.rt.color[j]->d.height, 1u) != g_pass.th) { PD("F %llu 证不出：附件不是第 0 级第 0 层或尺寸不符", g_stat_pass); return 0; }
            {   // The sampled view must also be exactly the attachment itself (previously only the attachment being level 0 layer 0 was checked, not the view bound by the shader -- when the same texture is sampled through a view of level k / layer k / a changed format,
                // the proof would still hold, the sampling would be changed to read on-tile level 0 / the original format, and the picture would be wrong). The view's starting level / layer must be 0 and its format must equal the attachment's actual format, otherwise do not merge
                int tid_ = (int)(h & 0xfffff); void *tv_ = NULL; if (tid_) tex_fast_rid(tid_, &tv_); __unsafe_unretained id<MTLTexture> tv = (__bridge id<MTLTexture>)tv_;
                uint32_t apf_ = tv.parentTexture ? (uint32_t)tv.parentTexture.pixelFormat : (uint32_t)tv.pixelFormat; uint8_t swz_[4];
                if (R.rt.has_cview[j] && R.rt.cview[j].set_format && !(nvn_fmt_to_mtl(R.rt.cview[j].format, &apf_, swz_) && apf_)) apf_ = 0;   // The attachment is attached through a format-changing view: take that format; if unrecognized, do not merge
                MTLPixelFormat apf = (MTLPixelFormat)apf_;
                if (!tv || (tv.parentTexture && (tv.parentRelativeLevel || tv.parentRelativeSlice)) || tv.pixelFormat != apf) {
                    static int lg; if (lg++ < 8) LOG("[nvn-mtl] 片上读合并：槽 %d 采样视图（起始级 %lu 层 %lu 格式 %lu）与附件（第 0 级第 0 层 格式 %lu）不符，不合并（帧 %llu 通道 %llu）", b->reg,
                        tv && tv.parentTexture ? (unsigned long)tv.parentRelativeLevel : 0ul, tv && tv.parentTexture ? (unsigned long)tv.parentRelativeSlice : 0ul, (unsigned long)tv.pixelFormat, (unsigned long)apf,
                        (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass);
                    PD("F %llu 证不出：槽 %d 采样视图不是附件那一张（级 / 层 / 格式不符）", g_stat_pass, b->reg); return 0; } }
            int any = 0;
            for (int c = 0; c < fs->rf.nfc; c++) if (fs->rf.fc_reg[c] == b->reg) {
                if (!uv_proof(vs, fs->rf.fc_locn[c], dw, di)) { PD("F %llu 证不出：槽 %d 的采样坐标不是本像素（插值输出 %d）", g_stat_pass, b->reg, fs->rf.fc_locn[c]); return 0; }
                fx[c] = j; fbmask |= (uint8_t)(1u << j); any = 1; }
            if (!any) { PD("F %llu 证不出：槽 %d 的采样不在可改表里", g_stat_pass, b->reg); return 0; } } }
    memset(lmap, 0xff, 8);
    for (int c = 0; c < 8; c++) if (fs->rf.color_mask >> c & 1) { if (g_fz.p2map[c] < 0) { PD("F %llu 证不出：输出颜色 %d 无对应附件", g_stat_pass, c); return 0; } lmap[g_fz.p2map[c]] = (uint8_t)c; }
    int ident = !fbmask; for (int c = 0; c < 8; c++) if ((fs->rf.color_mask >> c & 1) && g_fz.p2map[c] != c) ident = 0;
    if (ident) { for (int q = 0; q < 8; q++) if (lmap[q] != 0xff) g_fz.wrote[q] = 1; if (fz_check_on()) fz_snap_take(dw, di); return 1; }   // Does not read the tile and the output is not shifted: use the original version (the write mask still follows the mapping)
    if (!fs->rf.nfo) { bf_want_note(fs->key); PD("F %llu 证不出：没有可读 tile 版本（输出须移位）", g_stat_pass); return 0; }
    *fn = bf_fn(fs, fx, fbmask, lmap);
    if (!*fn) return 0;
    memset(&g_fz_desc, 0, sizeof g_fz_desc); g_fz_desc.kind = 1; g_fz_desc.fbmask = fbmask; for (int i = 0; i < 32; i++) g_fz_desc.fx[i] = (int8_t)fx[i];
    for (int q = 0; q < 8; q++) if (lmap[q] != 0xff) g_fz.wrote[q] = 1;
    if (fz_check_on()) fz_snap_take(dw, di);
    g_fz_draws++; return 1;
}
static void bloomq_reset(void) { memset(&g_bloomq, 0, sizeof g_bloomq); }
static int bloomq_enabled(void) {
    if (g_bloom_q < 0) { const char *e = getenv("DM_NVN_BLOOM_Q"); g_bloom_q = !e || strcmp(e, "0"); }
    return g_bloom_q;
}
static int bloomq_target(uint64_t *ser, int *lv, int *sl) {
    if (!R.has_rt || R.rt.n != 1 || !R.rt.color[0]) return 0;
    rt_sub(&R.rt, 0, ser, lv, sl); return *ser != 0;
}
static int bloomq_make_pipeline(MTLPixelFormat fmt) {
    if (g_bloomq_pso && g_bloomq_fmt == fmt) return 1;
    static const char *src =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; float2 uv; };\n"
        "vertex V vs(uint id [[vertex_id]]) { float2 uv=float2((id<<1)&2,id&2); V o; o.p=float4(uv*float2(2,-2)+float2(-1,1),0,1); o.uv=uv; return o; }\n"
        "struct P { float threshold; float coeff_g; float coeff_b; float dm; float sh; };\n"
        "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant P& p [[buffer(0)]]) { float2 d=float2(1.0/p.dm,1.0/p.sh); float3 c=0; for (int y=-1;y<=1;y+=2) for (int x=-1;x<=1;x+=2) { float3 v=t.sample(s,in.uv+float2(x,y)*d).rgb; float l=v.r*0.2126+v.g*p.coeff_g+v.b*p.coeff_b; if(l>=p.threshold) c+=v/(1.0+l); } return float4(c*0.25,1); }\n";
    NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src) options:nil error:&e];
    if (!lib) { MLOG("【红】泛光 1/4 着色器编译失败：%s", e.localizedDescription.UTF8String); return 0; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"]; pd.colorAttachments[0].pixelFormat = fmt;
    g_bloomq_pso = nvn_cached_render(g_dev, pd, &e);
    if (!g_bloomq_pso) { MLOG("【红】泛光 1/4 管线失败：%s", e.localizedDescription.UTF8String); return 0; }
    g_bloomq_fmt = fmt;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear; sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge; sd.supportArgumentBuffers = YES;
    g_bloomq_samp = [g_dev newSamplerStateWithDescriptor:sd]; return g_bloomq_samp != nil;
}
static int bloomq_capture(void) {
    uint64_t ser; int lv, sl; if (!bloomq_target(&ser, &lv, &sl) || !g_bloomq.s_tex) return 0;
    id<MTLTexture> base = tex_mtl(R.rt.color[0]), src = tex_mtl(g_bloomq.s_tex); if (!base || !src) return 0;
    id<MTLTexture> dst = base; if (lv || sl) { static id<MTLTexture> vb; static int vl, vs; if (vb != base || vl != lv || vs != sl) { g_bloomq_view = [base newTextureViewWithPixelFormat:base.pixelFormat textureType:base.textureType levels:NSMakeRange(lv, 1) slices:NSMakeRange(sl, 1)]; if (g_bloomq_view) res_add(g_bloomq_view); vb = base; vl = lv; vs = sl; } dst = g_bloomq_view; }
    if (!dst || !bloomq_make_pipeline(dst.pixelFormat)) return 0;
    rp_end();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new]; rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> en = enc_r_begin(rp); if (!en) return 0;

    [en setFragmentTexture:src atIndex:0]; [en setFragmentSamplerState:g_bloomq_samp atIndex:0];
    float *p = g_bloomq_const[g_bloomq_i].contents; p[0] = g_bloomq.threshold; p[1] = g_bloomq.coeff_g; p[2] = g_bloomq.coeff_b; p[3] = src.width; p[4] = src.height; [en setFragmentBuffer:g_bloomq_const[g_bloomq_i] offset:0 atIndex:0];
    [en setViewport:(MTLViewport){0,0,dst.width,dst.height,0,1}]; [en setRenderPipelineState:g_bloomq_pso]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; m4_end(en);
    g_bloomq_i = (g_bloomq_i + 1) % 3; args_dirty_all(); g_pso_dirty = g_dss_dirty = 1; return 1;
}
// ---------- Drag-follow image for shop items: items on a vendor's shelf have no "picked up" state, so during a drag the replay layer takes "the draw that draws this item's icon", translates it to the finger at the end of the same pass and draws it once more ----------
// In the UI pass each rectangle is one 4-vertex draw (20-byte vertices: position 3 floats + texture coordinates 2 floats; the position goes to clip coordinates through the matrix in vertex constant 0).
// The front end (front_ios.m) gives the point where the finger pressed, the shelf grid and the center of the drag-follow image (normalized, top-left origin); here, in the UI pass, find the rectangle that "covers this point, has all four edges on grid lines, and has the smallest area" = that item
// (identifying by the point alone gets it wrong: about 0.1 seconds after the press the game draws a one-cell-sized rectangle centered on the press point, not on grid lines -- on a real device it was twice taken for the item)
// (does not depend on the game's selected state: selection has to wait about 0.1 seconds for the controller cursor to land on the item, and a quick drag that starts on press cannot wait for that). The same rectangle has two draws, cell background and icon:
// the cell background uses a common texture (many rectangles in this pass use it), while each item has its own icon texture -- take the draw with the texture "used the fewest times in this pass". The recognized rectangle is reported back to the front end (used to cover the original shelf position with a dark color).
// Vertical position and height of the UI rectangle in the real picture (both as a fraction of the picture height: y = 0 touches the top, h = 1 is the whole picture); registered by the front end and read fresh on every draw (changes when switching between docked / handheld mode).
// The game draws the UI into the upper h portion of the UI texture (the viewport is the whole texture, and the projection matrix carries the scaling).
static void (*g_ui_rect_fn)(double *y, double *h);
void mtl_set_ui_rect_fn(void (*fn)(double *y, double *h)) { g_ui_rect_fn = fn; }
static double ui_hfrac(void) { double y = 0, h = 1; if (g_ui_rect_fn) g_ui_rect_fn(&y, &h); return h > 0 && h < 1 ? h : 1; }
static _Atomic int g_ghost_on; static _Atomic uint64_t g_ghost_pt, g_ghost_at, g_ghost_found, g_ghost_grid;
void mtl_ui_ghost(int on, float px, float py, const float grid[4], float cx, float cy) {
    if (on) { uint64_t gv = 0; for (int i = 0; i < 4; i++) gv |= (uint64_t)(uint16_t)(fminf(fmaxf(grid[i], 0), 1) * 65535 + .5f) << (16 * i); atomic_store(&g_ghost_grid, gv);
              atomic_store(&g_ghost_pt, (uint64_t)(uint32_t)lroundf(fminf(fmaxf(px, 0), 1) * 65535) | (uint64_t)(uint32_t)lroundf(fminf(fmaxf(py, 0), 1) * 65535) << 32);
              atomic_store(&g_ghost_at, (uint64_t)(uint32_t)(int32_t)lroundf(cx * 65535) | (uint64_t)(uint32_t)(int32_t)lroundf(cy * 65535) << 32); }
    else atomic_store(&g_ghost_found, 0);
    atomic_store(&g_ghost_on, on);
}
int mtl_ui_ghost_rect(float r[4]) { uint64_t v = atomic_load(&g_ghost_found); if (!v) return 0; for (int i = 0; i < 4; i++) r[i] = (float)((v >> (16 * i)) & 0xffff) / 65535.f; return 1; }
static struct { uint64_t pass; int n, nt, busy; float area, r[4]; struct { void *S_; p_draw dw; uint64_t tex; } c[4]; uint64_t tid[128]; uint16_t tn[128]; } g_gh;
static uint64_t ghost_tex(void) {   // The texture actually sampled by this draw's fragment shader (slot taken from shader reflection; the other slots in S.tex are left over from earlier draws)
    __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil; NvShader *f = pr ? pr->sh[1] : nil; if (!f) return 0;
    for (int i = 0; i < f->rf.nb; i++) if ((f->rf.b[i].kind == NVN_RB_TEX || f->rf.b[i].kind == NVN_RB_SAMP) && f->rf.b[i].reg < 128 && S.tex[1][f->rf.b[i].reg]) return S.tex[1][f->rf.b[i].reg];
    return 0; }
// Whether the current UI draw is "untextured and exactly covering the whole UI rectangle" (the UI rectangle occupies the upper hf of the texture)
static int ui_cover_all(double hf) {
    if (S.vbuf[0].size != 80 || ghost_tex()) return 0;
    const p_bind_buf *u = &S.ubo[0][0]; void *vb, *mb; uint64_t vo, mo; const uint8_t *vp_, *mp_;
    if (!u->addr || u->size < 64 || pend_hits(u->addr, 64) || pend_hits(S.vbuf[0].address, 80)) return 0;
    if (!mtl_addr(S.vbuf[0].address, 80, &vb, &vo) || !(vp_ = [(__bridge id<MTLBuffer>)vb contents]) || !mtl_addr(u->addr, 64, &mb, &mo) || !(mp_ = [(__bridge id<MTLBuffer>)mb contents])) return 0;
    float m[16], x0 = 9, y0 = 9, x1 = -9, y1 = -9; memcpy(m, mp_ + mo, 64);
    for (int i = 0; i < 4; i++) { float f[2]; memcpy(f, vp_ + vo + 20 * i, 8); float nx = (m[0] * f[0] + m[4] * f[1] + m[12] + 1) / 2, ny = (m[1] * f[0] + m[5] * f[1] + m[13] + 1) / 2;
        x0 = fminf(x0, nx); x1 = fmaxf(x1, nx); y0 = fminf(y0, ny); y1 = fmaxf(y1, ny); }
    return x0 < 0.002f && x1 > 0.998f && y0 < 0.002f && fabsf(y1 - (float)hf) < 0.002f;
}
static void ghost_note(const p_draw *dw) {
    if (g_gh.busy || dw->count != 4 || S.nattr != 2 || S.stream[0].stride != 20 || S.vbuf[0].size != 80) return;
    if (g_gh.pass != g_stat_pass) { for (int i = 0; i < g_gh.n; i++) free(g_gh.c[i].S_); g_gh.n = g_gh.nt = 0; g_gh.pass = g_stat_pass; }
    uint64_t t = ghost_tex(); int k = 0; for (; k < g_gh.nt; k++) if (g_gh.tid[k] == t) break;
    if (k == g_gh.nt && g_gh.nt < 128) { g_gh.tid[k] = t; g_gh.tn[k] = 0; g_gh.nt++; } if (k < g_gh.nt) g_gh.tn[k]++;
    const p_bind_buf *u = &S.ubo[0][0]; void *vb, *mb; uint64_t vo, mo; const uint8_t *vp_, *mp_;
    if (!u->addr || u->size < 64 || pend_hits(u->addr, 64) || pend_hits(S.vbuf[0].address, 80)) return;
    if (!mtl_addr(S.vbuf[0].address, 80, &vb, &vo) || !(vp_ = [(__bridge id<MTLBuffer>)vb contents]) || !mtl_addr(u->addr, 64, &mb, &mo) || !(mp_ = [(__bridge id<MTLBuffer>)mb contents])) return;
    float m[16], x0 = 9, y0 = 9, x1 = -9, y1 = -9; memcpy(m, mp_ + mo, 64);
    for (int i = 0; i < 4; i++) { float f[2]; memcpy(f, vp_ + vo + 20 * i, 8); float nx = (m[0] * f[0] + m[4] * f[1] + m[12] + 1) / 2, ny = (m[1] * f[0] + m[5] * f[1] + m[13] + 1) / 2;
        x0 = fminf(x0, nx); x1 = fmaxf(x1, nx); y0 = fminf(y0, ny); y1 = fmaxf(y1, ny); }
    { float hf = (float)ui_hfrac(); y0 /= hf; y1 /= hf; }   // The points given by the front end are normalized coordinates within the UI rectangle; when the UI occupies only the upper hf portion of the texture, convert to the same convention (unchanged when hf = 1)
    uint64_t pv = atomic_load(&g_ghost_pt); float qx = (float)(uint32_t)pv / 65535.f, qy = (float)(pv >> 32) / 65535.f;
    if (qx < x0 || qx > x1 || qy < y0 || qy > y1) return;
    uint64_t gv = atomic_load(&g_ghost_grid); float gx = (float)(gv & 0xffff) / 65535.f, gy = (float)((gv >> 16) & 0xffff) / 65535.f, gw = (float)((gv >> 32) & 0xffff) / 65535.f, gh = (float)(gv >> 48) / 65535.f;
    if (gw <= 0 || gh <= 0) return;
    float w = x1 - x0, h = y1 - y0, area = w * h, c0 = (x0 - gx) / gw, r0 = (y0 - gy) / gh, cn = w / gw, rn = h / gh;   // Which cell the top-left corner is in, and how many cells it occupies
    if (fabsf(c0 - roundf(c0)) > 0.15f || fabsf(r0 - roundf(r0)) > 0.15f || fabsf(cn - roundf(cn)) > 0.15f || fabsf(rn - roundf(rn)) > 0.15f || cn < 0.85f || rn < 0.85f || cn > 2.15f || rn > 4.15f) return;   // One item is at most 2x4 cells
    if (g_gh.n && area > g_gh.area * 1.03f) return;                       // A smaller one already exists
    if (g_gh.n && area < g_gh.area * 0.97f) { for (int i = 0; i < g_gh.n; i++) free(g_gh.c[i].S_); g_gh.n = 0; }   // Found a smaller one: the earlier ones are invalidated
    if (g_gh.n == 4) { free(g_gh.c[0].S_); memmove(&g_gh.c[0], &g_gh.c[1], 3 * sizeof g_gh.c[0]); g_gh.n = 3; }
    g_gh.area = area; g_gh.r[0] = x0; g_gh.r[1] = y0; g_gh.r[2] = x1; g_gh.r[3] = y1;
    typeof(g_gh.c[0]) *c = &g_gh.c[g_gh.n++]; c->S_ = malloc(sizeof S); memcpy(c->S_, &S, sizeof S); c->dw = *dw; c->tex = t;
}
static void ghost_flush(void) {   // Before the render pass ends: translate the recognized icon draw to the finger and draw it once more (moves the viewport, does not change vertices; the scissor is opened up)
    if (g_gh.busy || !g_gh.n) return;
    if (g_gh.pass != g_stat_pass || !atomic_load(&g_ghost_on)) { for (int i = 0; i < g_gh.n; i++) free(g_gh.c[i].S_); g_gh.n = 0; return; }
    int best = -1, bn = 1 << 30; for (int i = 0; i < g_gh.n; i++) { int cnt = 0; for (int k = 0; k < g_gh.nt; k++) if (g_gh.tid[k] == g_gh.c[i].tex) cnt = g_gh.tn[k]; if (cnt < bn) { bn = cnt; best = i; } }
    typeof(g_gh.c[0]) *c = &g_gh.c[best]; uint64_t at = atomic_load(&g_ghost_at); float ax = (float)(int32_t)(uint32_t)at / 65535.f, ay = (float)(int32_t)(uint32_t)(at >> 32) / 65535.f;
    float W = (float)g_pass.tw, H = (float)g_pass.th, ccx = (g_gh.r[0] + g_gh.r[2]) / 2, ccy = (g_gh.r[1] + g_gh.r[3]) / 2;
    { uint64_t v = 0; for (int i = 0; i < 4; i++) v |= (uint64_t)(uint16_t)(fminf(fmaxf(g_gh.r[i], 0), 1) * 65535 + .5f) << (16 * i); atomic_store(&g_ghost_found, v ? v : 1); }
    g_gh.busy = 1; typeof(S) save; memcpy(&save, &S, sizeof S); p_scissor sc = R.sc; int has_sc = R.has_sc;
    memcpy(&S, c->S_, sizeof S); if (!S.has_vp) { S.vp[0][0] = S.vp[0][1] = 0; S.vp[0][2] = W; S.vp[0][3] = H; S.has_vp = 1; }
    S.vp[0][0] += (ax - ccx) * W; S.vp[0][1] += (ay - ccy) * H * (float)ui_hfrac(); R.has_sc = 0;
    args_dirty_all(); g_pso_dirty = g_dss_dirty = 1; draw(&c->dw, NULL);
    { static int once; if (!once++) MLOG("拖货跟手图：首例 通道 %llu，候选 %d 个，取贴图本趟用到 %d 次的那次", g_stat_pass, g_gh.n, bn); }
    memcpy(&S, &save, sizeof S); args_dirty_all(); R.sc = sc; R.has_sc = has_sc; g_pso_dirty = g_dss_dirty = 1; g_gh.busy = 0;
    for (int i = 0; i < g_gh.n; i++) free(g_gh.c[i].S_); g_gh.n = 0;
}
// Diagnostic DM_NVN_KEYDUMP=<fragment shader key prefix>[:<count, default 6>[:<frame interval>]]: watch only this class of draws (for real devices; no frame number to guess). The first N matching draws force PASSDUMP's per-binding lines on (K / B / d:
// textures with format / view / channel swizzle, samplers with LOD clamps), plus [kd] lines: vertex attribute formats, each vertex stream (first 4 vertices of per-vertex streams, all instances of per-instance streams) and every value of each uniform buffer of both stages.
// Run the same switch once on the Mac and once on the device and compare item by item to see which input differs on the device (Super Mario 3D World + Bowser's Fury's magenta squares in 1-1: particle 5e4de7561c670fee, iPhone only, not reproducible on the Mac)
static void kd_floats(const char *what, int reg, uint64_t addr, uint64_t size, uint64_t cap) {
    void *mb; uint64_t mo; const uint8_t *base;
    if (!addr || !size) { fprintf(stderr, "[kd]   %s r%d 未绑\n", what, reg); return; }
    if (!mtl_addr(addr, 1, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) { fprintf(stderr, "[kd]   %s r%d addr=%#llx 不在内存池\n", what, reg, (unsigned long long)addr); return; }
    uint64_t n = MIN(size, cap) / 4; const float *v = (const float *)(base + mo);
    fprintf(stderr, "[kd]   %s r%d addr=%#llx size=%llu%s\n", what, reg, (unsigned long long)addr, (unsigned long long)size, pend_hits(addr, size) ? "（本提交里还有待执行的写）" : "");
    for (uint64_t i = 0; i < n; i += 16) {
        char line[1024]; size_t o = 0;
        for (uint64_t k = i; k < n && k < i + 16 && o < sizeof line - 24; k++) o += snprintf(line + o, sizeof line - o, " %.6g", v[k]);
        fprintf(stderr, "[kd]     +%#06llx%s\n", (unsigned long long)(i * 4), line);
    }
}
static int kd_draw(NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    static char key[24]; static int left = -1, every; static uint64_t last = ~0ull;
    if (left < 0) { left = 0; const char *e = getenv("DM_NVN_KEYDUMP");
        if (e && *e) { size_t n = strcspn(e, ":"); if (n > 23) n = 23; memcpy(key, e, n); key[n] = 0; const char *c = strchr(e, ':'); left = c ? atoi(c + 1) : 6; every = c && strchr(c + 1, ':') ? atoi(strchr(c + 1, ':') + 1) : 0;
            MLOG("DM_NVN_KEYDUMP：片元着色器 %s 的 %d 次绘制逐项记录%s（[kd] / [pass] 行）", key, left, every ? "，每隔这么多帧最多记一次" : ""); } }
    if (left <= 0 || !pr->sh[1] || strncmp(pr->sh[1]->key, key, strlen(key))) return 0;
    uint64_t fr = atomic_load(&g_cur_frame);
    if (every > 0 && last != ~0ull && fr < last + (uint64_t)every) return 0;   // at most once every `every` frames: samples spread over the session, no need to guess the frame of the scene
    last = fr; left--;
    int count = dw ? dw->count : di->count, inst = dw ? dw->instances : di->instances, first = dw ? dw->first : 0, binst = dw ? dw->base_instance : di->base_instance;
    fprintf(stderr, "[kd] F%llu 通道 %llu vs=%s fs=%s 图元 %d 顶点 %d 实例 %d 起点 %d 基顶点 %d 基实例 %d 目标 %s\n", (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass,
            pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[1]->key, dw ? dw->prim : di->prim, count, inst, first, di ? di->base_vertex : 0, binst, R.rt.n && R.rt.color[0] ? "有" : "无");
    for (int i = 0; i < S.nattr && i < 16; i++) fprintf(stderr, "[kd]   属性 %d 格式 %#x 偏移 %u 流 %u\n", i, (unsigned)S.attr[i].fmt, (unsigned)S.attr[i].offset, (unsigned)S.attr[i].stream);
    for (int s_ = 0; s_ < S.nstream && s_ < 16; s_++) {
        uint32_t st = S.stream[s_].stride, dv = S.stream[s_].divisor;
        fprintf(stderr, "[kd]   流 %d 步长 %u 除数 %u 缓冲 %#llx 大小 %llu\n", s_, st, dv, (unsigned long long)S.vbuf[s_].address, (unsigned long long)S.vbuf[s_].size);
        if (!S.vbuf[s_].address || !st) continue;
        uint64_t from = (uint64_t)(dv ? binst : (di ? di->base_vertex : first)) * st, n = (uint64_t)st * (dv ? (uint64_t)MIN(inst, 8) : 4);
        if (from < S.vbuf[s_].size) kd_floats("流", s_, S.vbuf[s_].address + from, MIN(n, S.vbuf[s_].size - from), 512);
    }
    for (int stg = 0; stg < 2; stg++) { NvShader *sh = pr->sh[stg]; if (!sh) continue;
        for (int i = 0; i < sh->rf.nb; i++) if (sh->rf.b[i].kind == NVN_RB_CBUF && sh->rf.b[i].reg < 16) {
            const p_bind_buf *u = &S.ubo[stg][sh->rf.b[i].reg]; kd_floats(stg ? "片元 uniform" : "顶点 uniform", sh->rf.b[i].reg, u->addr, u->size, 4096); } }
    return 1;
}
// Diagnostic DM_NVN_MAGSCAN=<fragment key>:<scan every N frames, default 30>:<frames per phase, default 600>: on-device self-check for the magenta squares (Super Mario 3D World + Bowser's Fury 1-1, iPhone only, not reproducible on the Mac).
// In a scan frame, as soon as each render pass ends, a compute kernel reads its color attachments (read goes through the same format decode as the game's shader samples) and gathers per 16x16 block: the magenta pixel count (R and B both >= 0.8 of the largest channel, G <= 0.1 of it),
// the non-finite pixel count and each channel's min / max. An attachment with a block holding one single magenta value throughout (exactly what the squares in the iPhone screenshot are, aligned to the 16-pixel grid) or with non-finite values gets a [mag] line (index of the pass within the frame, distance from the pass holding this particle, attachment serial / format / size, block position and value).
// Four phases rotate: phase 0 draws as usual, phase 1 drops every draw of this fragment shader, phases 2 / 3 keep the geometry and replace the fragment output with a constant (grey / bright magenta). Comparing the phases tells whether this draw causes it
// and whether its fragment output (texture reads, value range) matters; the first pass where the magenta blocks show tells which step goes wrong
static char g_ms_key[24]; static int g_ms_every, g_ms_plen, g_ms_init;
static int ms_on(void);
static const char *const k_ms_phase[4] = {"照常画", "丢掉该着色器", "该着色器改出常量灰", "该着色器改出常量高亮品红"};
static int ms_phase_now(void) { return (int)(atomic_load(&g_cur_frame) / (uint64_t)g_ms_plen % 4); }
// Phases 2 / 3: this shader's draws keep their geometry and the fragment stage is replaced by a constant output (grey 0.5 / bright magenta 1500,2,1500). The pipeline cache key carries the replacement function, kept apart from the original pipeline; these pipelines are not recorded in the prewarm list
static id<MTLFunction> ms_fs_override(NvShader *fs) {
    if (!fs || !ms_on() || strncmp(fs->key, g_ms_key, strlen(g_ms_key))) return nil;
    int ph = ms_phase_now(); if (ph < 2) return nil;
    static id<MTLFunction> fn[2]; static int failed;
    if (!fn[0] && !failed) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
            "fragment float4 msg() { return float4(0.5, 0.5, 0.5, 1.0); }\nfragment float4 msm() { return float4(1500.0, 2.0, 1500.0, 0.6); }\n" options:nil error:&e];
        fn[0] = [lib newFunctionWithName:@"msg"]; fn[1] = [lib newFunctionWithName:@"msm"];
        if (!fn[0] || !fn[1]) { failed = 1; MLOG("【红】DM_NVN_MAGSCAN：常量片元编不出 %s", e.localizedDescription.UTF8String); } }
    return failed ? nil : fn[ph - 2];
}
static int ms_on(void) {
    if (!g_ms_init) { g_ms_init = 1; const char *e = getenv("DM_NVN_MAGSCAN");
        if (e && *e) { size_t n = strcspn(e, ":"); if (n > 23) n = 23; memcpy(g_ms_key, e, n); g_ms_key[n] = 0;
            const char *c = strchr(e, ':'); g_ms_every = c ? atoi(c + 1) : 30; c = c ? strchr(c + 1, ':') : NULL; g_ms_plen = c ? atoi(c + 1) : 600;
            if (g_ms_every < 1) g_ms_every = 30; if (g_ms_plen < g_ms_every) g_ms_plen = 600;
            MLOG("DM_NVN_MAGSCAN：片元着色器 %s，每 %d 帧扫一次各通道颜色附件里的品红块；每 %d 帧换一相（相 0 照常画 / 1 丢掉这个着色器的绘制 / 2 它改出常量灰 / 3 它改出常量高亮品红）（[mag] 行）", g_ms_key, g_ms_every, g_ms_plen); } }
    return g_ms_key[0] != 0;
}
static uint64_t g_ms_ppass = ~0ull, g_ms_pframe = ~0ull, g_ms_drawn, g_ms_dropped;
// returns 1 = this phase drops the draw
static int ms_draw(NvProg *pr) {
    if (!ms_on() || !pr->sh[1] || strncmp(pr->sh[1]->key, g_ms_key, strlen(g_ms_key))) return 0;   // a deferred draw returns before this when recorded and only arrives here when replayed: every draw passes exactly once
    uint64_t f = atomic_load(&g_cur_frame);
    if (f / (uint64_t)g_ms_plen % 4 == 1) { g_ms_dropped++; return 1; }
    g_ms_drawn++; if (g_ms_pframe != f) { g_ms_pframe = f; g_ms_ppass = g_stat_pass; }   // the pass where this particle first draws this frame
    return 0;
}
static const char k_ms_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"kernel void ms(texture2d<float, access::read> t [[texture(0)]], device uint *o [[buffer(0)]], uint2 g [[thread_position_in_grid]], uint2 tg [[threadgroup_position_in_grid]],\n"
"               uint li [[thread_index_in_threadgroup]], uint si [[simdgroup_index_in_threadgroup]], uint sl [[thread_index_in_simdgroup]]) {\n"
"  threadgroup uint sh[8][8]; bool in = g.x < o[0] && g.y < o[1]; float4 c = in ? t.read(g) : float4(0); float m = max(c.r, max(c.g, c.b));\n"
"  bool bad = in && (isnan(c.r) || isnan(c.g) || isnan(c.b) || isinf(c.r) || isinf(c.g) || isinf(c.b));\n"
"  bool mag = in && !bad && m > 0.0 && c.r >= 0.8 * m && c.b >= 0.8 * m && c.g <= 0.1 * m;\n"
"  uint r = as_type<uint>(max(c.r, 0.0)), gg = as_type<uint>(max(c.g, 0.0)), b = as_type<uint>(max(c.b, 0.0));\n"
"  uint v[8] = {simd_sum(mag ? 1u : 0u), simd_sum(bad ? 1u : 0u), simd_min(in ? r : ~0u), simd_max(in ? r : 0u), simd_min(in ? gg : ~0u), simd_max(in ? gg : 0u), simd_min(in ? b : ~0u), simd_max(in ? b : 0u)};\n"
"  if (sl == 0) for (int k = 0; k < 8; k++) sh[si][k] = v[k];\n"
"  threadgroup_barrier(mem_flags::mem_threadgroup); if (li) return;\n"
"  for (int s = 1; s < 8; s++) { v[0] += sh[s][0]; v[1] += sh[s][1]; for (int k = 2; k < 8; k += 2) { v[k] = min(v[k], sh[s][k]); v[k + 1] = max(v[k + 1], sh[s][k + 1]); } }\n"
"  device uint *q = o + 4 + 4 * (tg.y * o[2] + tg.x); q[0] = v[0] | v[1] << 16 | (v[2] == v[3] && v[4] == v[5] && v[6] == v[7] ? 1u << 31 : 0u); q[1] = v[3]; q[2] = v[5]; q[3] = v[7]; }\n";
#define MS_BUF (16u << 20)
#define MS_MAXE 256
typedef struct { uint64_t pass_idx, ser; int att, pdist, fmt; uint32_t w, h, off; } ms_ent;
typedef struct { __strong id<MTLBuffer> b; uint64_t frame, fence; uint32_t used; int n, busy, full; ms_ent e[MS_MAXE]; } ms_scan;
static ms_scan g_ms_s[2]; static int g_ms_cur = -1;   // which buffer this scan frame writes (-1 = not scanning this frame)
static uint64_t g_ms_curf = ~0ull, g_ms_first;
static __strong id<MTLTexture> g_ms_att[8]; static NSUInteger g_ms_lv[8], g_ms_sl[8]; static uint64_t g_ms_aser[8]; static int g_ms_na; static uint64_t g_ms_apass;
static int g_ms_seq = -1, g_ms_sc, g_ms_hitf, g_ms_maxblk, g_ms_pscan; static uint64_t g_ms_f0, g_ms_f1, g_ms_pmag;   // the phase being tallied (frame / frames per phase) and its totals
static void ms_poll(void) {
    for (int k = 0; k < 2; k++) { ms_scan *s = &g_ms_s[k]; if (!s->busy || g_ms_cur == k || s->fence > g_done) continue;
        int seq = (int)(s->frame / (uint64_t)g_ms_plen);
        if (seq != g_ms_seq) {
            if (g_ms_seq >= 0) MLOG("[mag] 相 %d（%s）帧 %llu–%llu：扫 %d 帧，有均一品红块（16×16 整块同一个品红值）的 %d 帧，单帧最多 %d 块；粒子所在通道的附件里品红像素 每帧均 %.1f（%d 帧）；该着色器至今画 %llu 次、丢 %llu 次",
                                    g_ms_seq % 4, k_ms_phase[g_ms_seq % 4], (unsigned long long)g_ms_f0, (unsigned long long)g_ms_f1, g_ms_sc, g_ms_hitf, g_ms_maxblk, g_ms_pscan ? (double)g_ms_pmag / g_ms_pscan : 0.0, g_ms_pscan,
                                    (unsigned long long)g_ms_drawn, (unsigned long long)g_ms_dropped);
            g_ms_seq = seq; g_ms_sc = g_ms_hitf = g_ms_maxblk = g_ms_pscan = 0; g_ms_pmag = 0; g_ms_f0 = s->frame; }
        g_ms_sc++; g_ms_f1 = s->frame; int fblk = 0, pseen = 0;
        for (int i = 0; i < s->n; i++) { const ms_ent *e = &s->e[i]; const uint32_t *o = (const uint32_t *)((const uint8_t *)s->b.contents + e->off);
            uint32_t tw = (e->w + 15) / 16, th = (e->h + 15) / 16; uint64_t bad = 0, mag = 0; int ublk = 0, mblk = 0, nl = 0; char line[1400]; size_t lo = 0;
            for (uint32_t t = 0; t < tw * th; t++) { const uint32_t *q = o + 4 + 4 * t; uint32_t nmag = q[0] & 0xffff, nbad = q[0] >> 16 & 0x7fff; bad += nbad; mag += nmag;
                uint32_t npx = MIN(16u, e->w - t % tw * 16) * MIN(16u, e->h - t / tw * 16);
                int uni = q[0] >> 31;   // the whole block holds one value
                int allm = nmag == npx;
                if (allm) mblk++; if (allm && uni && npx >= 64) ublk++;
                if (((allm && uni && npx >= 64) || nbad) && nl < 8 && lo < sizeof line - 120) { float r, g, b; memcpy(&r, &q[1], 4); memcpy(&g, &q[2], 4); memcpy(&b, &q[3], 4); nl++;
                    lo += snprintf(line + lo, sizeof line - lo, " [%u,%u%s 最大值 %g %g %g 非有限 %u]", t % tw * 16, t / tw * 16, uni ? " 均一" : "", r, g, b, nbad); } }
            fblk += ublk; if (e->pdist == 0 && e->att == 0) { g_ms_pmag += mag; pseen = 1; }
            if (ublk || bad) MLOG("[mag] F%llu 相 %llu 通道 %llu（粒子通道%+d）附件 %d ser=%llu 格式 %d %ux%u：均一品红块 %d（全品红块 %d），非有限 %llu 像素%s", (unsigned long long)s->frame, (unsigned long long)(s->frame / (uint64_t)g_ms_plen % 4),
                                  (unsigned long long)e->pass_idx, e->pdist, e->att, (unsigned long long)e->ser, e->fmt, e->w, e->h, ublk, mblk, (unsigned long long)bad, line);
        }
        if (fblk) g_ms_hitf++; if (fblk > g_ms_maxblk) g_ms_maxblk = fblk; g_ms_pscan += pseen;
        if (s->full) MLOG("[mag] F%llu 扫描缓冲满，本帧后面的通道没扫", (unsigned long long)s->frame);
        s->busy = 0; s->n = 0; s->used = 0; s->full = 0; }
}
void ms_note(MTLRenderPassDescriptor *pd, NSUInteger nc) {
    g_ms_na = 0; if (!ms_on()) return;
    uint64_t f = atomic_load(&g_cur_frame);
    if (f != g_ms_curf) {   // a new frame: the previous scan frame's counts are processed once its submission completes
        g_ms_curf = f; g_ms_first = g_stat_pass + 1; if (g_ms_cur >= 0) g_ms_cur = -1;
        if (f % (uint64_t)g_ms_every == 0) { for (int k = 0; k < 2; k++) if (!g_ms_s[k].busy) { g_ms_cur = k; break; }
            if (g_ms_cur >= 0) { ms_scan *s = &g_ms_s[g_ms_cur];
                if (!s->b) { s->b = [g_dev newBufferWithLength:MS_BUF options:MTLResourceStorageModeShared]; if (!s->b) { g_ms_cur = -1; return; } res_add(s->b); }
                s->busy = 1; s->frame = f; s->n = 0; s->used = 0; s->full = 0; s->fence = 0; } } }
    ms_poll();
    if (g_ms_cur < 0) return;
    g_ms_apass = g_stat_pass + 1;
    for (NSUInteger i = 0; i < nc && i < 8; i++) { id<MTLTexture> t = pd.colorAttachments[i].texture; if (!t) continue;
        g_ms_att[g_ms_na] = t; g_ms_lv[g_ms_na] = pd.colorAttachments[i].level; g_ms_sl[g_ms_na] = pd.colorAttachments[i].slice; g_ms_aser[g_ms_na] = R.rt.color[i] ? R.rt.color[i]->serial : 0; g_ms_na++; }
}
void ms_after(void) {
    if (!g_ms_na) return;
    int na = g_ms_na; g_ms_na = 0;
    if (g_ms_cur < 0 || g_stat_pass != g_ms_apass) { for (int i = 0; i < na; i++) g_ms_att[i] = nil; return; }
    static int failed; static id<MTLComputePipelineState> ps; static int logged_att;
    if (failed) return;
    if (!ps) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_ms_msl) options:nil error:&e]; id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"ms"] : nil;
        ps = fn ? nvn_cached_compute(g_dev, fn, &e) : nil;
        if (!ps) { failed = 1; MLOG("【红】DM_NVN_MAGSCAN：计算管线建不出 %s", e.localizedDescription.UTF8String); return; } }
    ms_scan *s = &g_ms_s[g_ms_cur]; int pdist = g_ms_pframe == s->frame && g_ms_ppass != ~0ull ? (int)((int64_t)g_stat_pass - (int64_t)g_ms_ppass) : 9999;
    for (int i = 0; i < na; i++) { id<MTLTexture> t = g_ms_att[i]; g_ms_att[i] = nil;
        MTLPixelFormat pf = t.pixelFormat;
        int ok = pf == MTLPixelFormatRGBA8Unorm || pf == MTLPixelFormatRGBA8Unorm_sRGB || pf == MTLPixelFormatBGRA8Unorm || pf == MTLPixelFormatBGRA8Unorm_sRGB || pf == MTLPixelFormatRGB10A2Unorm
              || pf == MTLPixelFormatRG11B10Float || pf == MTLPixelFormatRGBA16Float || pf == MTLPixelFormatRGBA16Unorm || pf == MTLPixelFormatRGBA32Float;
        if (!ok || !(t.usage & MTLTextureUsageShaderRead) || t.sampleCount > 1 || (t.width >> g_ms_lv[i]) < 64 || t.storageMode == MTLStorageModeMemoryless || (t.textureType != MTLTextureType2D && t.textureType != MTLTextureType2DArray)) continue;
        if (pdist == 0 && !logged_att++) MLOG("[mag] 粒子所在通道的附件 %d：ser=%llu 格式 %lu %lux%lu 级 %lu 层 %lu 存储 %lu 用途 %#lx 压缩类型 %ld 允许 GPU 优化排布 %d", i, (unsigned long long)g_ms_aser[i], (unsigned long)pf,
                                             (unsigned long)t.width, (unsigned long)t.height, (unsigned long)g_ms_lv[i], (unsigned long)g_ms_sl[i], (unsigned long)t.storageMode, (unsigned long)t.usage, (long)t.compressionType, (int)t.allowGPUOptimizedContents);
        uint32_t w = (uint32_t)MAX(t.width >> g_ms_lv[i], 1ul), h = (uint32_t)MAX(t.height >> g_ms_lv[i], 1ul), tw = (w + 15) / 16, th = (h + 15) / 16, need = 16 + tw * th * 16;
        if (s->n >= MS_MAXE || s->used + need > MS_BUF) { s->full = 1; continue; }
        id<MTLTexture> v = t;
        if (t.textureType != MTLTextureType2D || g_ms_lv[i] || g_ms_sl[i]) v = [t newTextureViewWithPixelFormat:pf textureType:MTLTextureType2D levels:NSMakeRange(g_ms_lv[i], 1) slices:NSMakeRange(g_ms_sl[i], 1)];
        if (!v) continue;
        uint32_t *o = (uint32_t *)((uint8_t *)s->b.contents + s->used); o[0] = w; o[1] = h; o[2] = tw; o[3] = 0;
        ms_ent *e = &s->e[s->n++]; e->pass_idx = g_stat_pass - g_ms_first; e->ser = g_ms_aser[i]; e->att = i; e->pdist = pdist; e->fmt = (int)pf; e->w = w; e->h = h; e->off = s->used; s->used += need;
        if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
        id<MTLComputeCommandEncoder> ce = (id<MTLComputeCommandEncoder>)enc_c_begin(NULL);
        [ce setTexture:v atIndex:0]; [ce setBuffer:s->b offset:e->off atIndex:0];
        enc_c_pso(ce, ps); enc_c_dispatch(ce, MTLSizeMake(tw, th, 1), MTLSizeMake(16, 16, 1)); enc_c_end(ce);
        args_dirty_all(); s->fence = g_fence + 1;
        if (v != t) defer_obj_unref(v);
    }
}
// Same-pass feedback sampling: a texture view this draw's fragment stage samples is exactly the level and layer of a color attachment of the current render pass (Super Mario 3D World + Bowser's Fury:
// 66a71a3081b91b9a writes the tone-mapped 1920x1080 RGB10A2 image as an attachment while sampling it as a texture). On the Switch (Maxwell, immediate-mode) the read returns the content from before this draw;
// on Metal, writing and sampling the same attachment within one render pass is undefined: the Mac happens to read correctly, while the iPhone (A20 Pro) now and then reads whole 16x16 blocks as (1,0,1) and FXAA carries them into the picture - the magenta squares in 1-1
// (measured with DM_NVN_MAGSCAN: the squares first appear on this attachment, and still appear with the particle dropped).
// The fix: before this draw opens its render pass, end the current render pass (the attachment lands in memory), copy that level and layer into a snapshot texture in the replay stream, make this draw sample the snapshot; later draws reopen the render pass as usual (loading the attachment).
// Only single-level 2D views that are exactly the attachment's level and layer are handled; other shapes are reported once and left as they were. Done only for the fragment shaders the game profile's feedback_snap lists (see fb_want)
// Which fragment shaders get it: the game profile's feedback_snap (key prefixes, space-separated); DM_NVN_FB_SNAP=0 none, all every shader, <keys,...> another list. The cost of doing all:
// Breath of the Wild has over a thousand draws a frame sampling the G-buffer of their own pass (frame time 16.6 -> 36.6 ms) and Mario Wonder's submit time went 1.4 -> 4.0 ms, so only shaders seen to break are listed
static char g_fb_keys[16][20]; static int g_fb_nk = -1, g_fb_all;
static int fb_want(const char *key) {
    if (g_fb_nk < 0) { g_fb_nk = 0; const char *e = getenv("DM_NVN_FB_SNAP"), *src = e && *e ? e : DM_GAME_FEEDBACK_SNAP;
        if (!strcmp(src, "all")) g_fb_all = 1;
        else if (strcmp(src, "0")) for (const char *q = src; *q && g_fb_nk < 16;) { size_t n = strcspn(q, ", "); if (n) { if (n > 16) n = 16; memcpy(g_fb_keys[g_fb_nk], q, n); g_fb_keys[g_fb_nk++][n] = 0; } q += n; q += strspn(q, ", "); }
        if (g_fb_all || g_fb_nk) MLOG("同通道反馈采样快照：%s%s", g_fb_all ? "全部片元着色器" : src, e && *e ? "（DM_NVN_FB_SNAP）" : "（游戏档案 feedback_snap）"); }
    if (g_fb_all) return 1;
    for (int i = 0; i < g_fb_nk; i++) if (!strncmp(key, g_fb_keys[i], strlen(g_fb_keys[i]))) return 1;
    return 0;
}
static id<MTLTexture> fb_snap_tex(MTLPixelFormat pf, NSUInteger w, NSUInteger h) {   // one per (format, size); full queue barriers separate consecutive snapshots, so reuse within a frame does not conflict
    static __strong id<MTLTexture> pool[4]; static int next;
    for (int i = 0; i < 4; i++) if (pool[i] && pool[i].pixelFormat == pf && pool[i].width == w && pool[i].height == h) return pool[i];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView; d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d]; if (!t) return nil; res_add(t);
    if (pool[next]) defer_obj_release(pool[next]); pool[next] = t; next = (next + 1) & 3;
    return t;
}
// Called before the draw opens its render pass: if a snapshot is needed, end the current render pass, copy the snapshot and register the substitution (used by this draw's argument encoding)
static void fb_prepare(NvProg *pr) {
    g_draw_seq++; g_fb_n = 0;
    if (g_fb_rebind) { args_dirty_all(); g_fb_rebind = 0; }   // the previous draw had a substituted binding: re-encode with the original bindings this time
    if (!pr || !pr->sh[1] || !R.rt.n || g_fz.active || g_defer_replaying || !fb_want(pr->sh[1]->key)) return;
    NvShader *fs = pr->sh[1];
    for (int i = 0; i < fs->rf.nb && g_fb_n < 4; i++) { const nvn_rbind *b = &fs->rf.b[i]; if (b->kind != NVN_RB_TEX && b->kind != NVN_RB_TEXU) continue;
        int tid = b->kind == NVN_RB_TEXU ? (int)(ubo_handle(1, b->reg, b->off) & 0xfffff) : b->reg < NTEX ? (int)(S.tex[1][b->reg] & 0xfffff) : 0; if (!tid) continue;
        pthread_mutex_lock(&g_rmu); uint64_t ser = tid < g_ntreg && g_treg[tid].ok ? g_treg[tid].t.serial : 0; pthread_mutex_unlock(&g_rmu);
        if (!ser) continue;
        for (int k = 0; k < R.rt.n && k < 8 && g_fb_n < 4; k++) { if (!R.rt.color[k] || R.rt.color[k]->serial != ser) continue;
            uint64_t aser; int lv, sl; rt_sub(&R.rt, k, &aser, &lv, &sl);
            pthread_mutex_lock(&g_rmu); void *tv = NULL; tex_fast_rid(tid, &tv); id<MTLTexture> t = (__bridge id<MTLTexture>)tv; pthread_mutex_unlock(&g_rmu);
            if (!t) continue;
            int dup = 0; for (int q = 0; q < g_fb_n; q++) dup |= g_fb_src[q] == t; if (dup) continue;
            id<MTLTexture> base = t.parentTexture ? t.parentTexture : t; NSUInteger tl = t.parentTexture ? t.parentRelativeLevel : 0, ts = t.parentTexture ? t.parentRelativeSlice : 0;
            if (tl + t.mipmapLevelCount <= (NSUInteger)lv || tl > (NSUInteger)lv) continue;   // the sampled levels do not include the attachment's level (mip generation etc.): not feedback
            if (t.textureType != MTLTextureType2D || t.mipmapLevelCount != 1 || t.sampleCount != 1 || ts != (NSUInteger)sl || b->dtt != MTLTextureType2D || base.parentTexture) {
                RED_ONCE(80, "同通道反馈采样：采样视图不是附件那一级那一层的单级二维视图（类型 %lu 级 %lu+%lu 层 %lu，附件 级 %d 层 %d，着色器 %s），照旧", (unsigned long)t.textureType, (unsigned long)tl, (unsigned long)t.mipmapLevelCount, (unsigned long)ts, lv, sl, fs->key);
                continue; }
            NSUInteger w = MAX(base.width >> lv, 1ul), h = MAX(base.height >> lv, 1ul);
            id<MTLTexture> snap = fb_snap_tex(base.pixelFormat, w, h); if (!snap) continue;
            if (g_enc) rp_end();
            id<MTLBlitCommandEncoder> e = blit(); enc_b_copy_texture(e, base, (NSUInteger)sl, (NSUInteger)lv, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, 1), snap, 0, 0, MTLOriginMake(0, 0, 0));
            MTLTextureSwizzleChannels sw = t.swizzle;
            int same = t.pixelFormat == snap.pixelFormat && sw.red == MTLTextureSwizzleRed && sw.green == MTLTextureSwizzleGreen && sw.blue == MTLTextureSwizzleBlue && sw.alpha == MTLTextureSwizzleAlpha;
            id<MTLTexture> use = same ? snap : [snap newTextureViewWithPixelFormat:t.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1) swizzle:sw];
            if (!use) { RED_ONCE(81, "同通道反馈采样：快照视图建不出（格式 %lu），照旧", (unsigned long)t.pixelFormat); continue; }
            if (g_fb_use[g_fb_n] && g_fb_use[g_fb_n] != use) defer_obj_unref(g_fb_use[g_fb_n]);
            g_fb_src[g_fb_n] = t; g_fb_use[g_fb_n] = use; g_fb_rid[g_fb_n] = legacy_texture_rid(use); g_fb_n++;
            g_fb_count++;
            if (g_fb_count == 1 || g_fb_count == 10 || g_fb_count == 1000 || g_fb_count % 100000 == 0)
                MLOG("同通道反馈采样：快照第 %llu 次（帧 %llu 通道 %llu，着色器 %s 槽 %d 采样附件 %d ser=%llu %lux%lu 格式 %lu）", (unsigned long long)g_fb_count, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass + !g_enc,
                     fs->key, b->reg, k, (unsigned long long)ser, (unsigned long)w, (unsigned long)h, (unsigned long)base.pixelFormat);
            PD("N %llu 同通道反馈采样：附件 %d ser=%llu 级 %d 层 %d 拷进快照，本次绘制改采样快照", g_stat_pass + !g_enc, k, (unsigned long long)ser, lv, sl);
        } }
    if (g_fb_n) { g_fb_seq = g_draw_seq; args_dirty_all(); g_fb_rebind = 1; }
}
// Whether this draw needs more from the inline updates deferred in the open pass than the uniform shadows give (see mtl_sub_update): a uniform binding they cover only in part, a bindless
// handle in such a part, or their data read as vertices, indices, indirect arguments or a storage buffer. Then the pass ends first and the updates land before the draw, as before
static int ubd_conflict(NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    for (int s_ = 0; s_ < 16; s_++) if (S.vbuf[s_].address && ubd_hits(S.vbuf[s_].address, S.vbuf[s_].address + S.vbuf[s_].size)) return 1;
    if (di && di->ibuf && ubd_hits(di->ibuf, di->ibuf + (uint64_t)di->count * 4)) return 1;
    uint64_t ind = dw ? dw->ind : di ? di->ind : 0;
    if (ind && ubd_hits(ind, ind + (dw ? sizeof(MTLDrawPrimitivesIndirectArguments) : sizeof(MTLDrawIndexedPrimitivesIndirectArguments)))) return 1;
    for (int st = 0; pr && st < 2; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh) continue;   // the stages encode_args binds (vertex, fragment)
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; if (b->reg >= 16) continue;
            const p_bind_buf *u = b->kind == NVN_RB_SBUFP ? &S.ssbo[st][b->reg] : &S.ubo[st][b->reg];
            if (!u->addr) continue;
            if (b->kind == NVN_RB_CBUF) { if (ubd_hits(u->addr, u->addr + u->size) && !ubd_covers(u->addr, u->addr + u->size)) return 1; }
            else if (b->kind == NVN_RB_TEXU || b->kind == NVN_RB_SAMPU) { uint64_t h = u->addr + b->off; if (ubd_hits(h, h + 4) && !ubd_covers(h, h + 4)) return 1; }
            else if (b->kind == NVN_RB_SBUFP || b->kind == NVN_RB_UBOP) { if (ubd_hits(u->addr, u->addr + (u->size ? u->size : 1))) return 1; } } }
    return 0;
}

// NVN transform feedback is used by Pokémon Legends: Z-A as a vertex-only
// skinning pass. Metal has no native feedback stage, so the translated
// main0_tf vertex entry writes to four ordinary buffers while rasterization is
// disabled. Later character draws consume those buffers as vertex streams.
enum { TF_SLOT = 8 };
static __unsafe_unretained id<MTLRenderPipelineState> tf_pso(NvShader *vs) {
    pso_key key; static const uint32_t no_formats[8];
    pso_key_fill(&key, vs, nil, no_formats, 0, 0, 1, 0); key.topology = MTLPrimitiveTopologyClassPoint; key.fsfn = TF_MARK;
    if (!g_bare_native) key.cfmt[0] = MTLPixelFormatRGBA16Float;
    __unsafe_unretained id<MTLRenderPipelineState> cached = hm_get_id(&g_pso, &key);
    if (cached) return cached;
    if (hm_get_id(&g_pso_fail, &key)) return nil;
    NSError *error = nil;
    id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithDescriptor:pso_desc(&key, vs, nil, nil, 0) error:&error];
    if (!pipeline) {
        MLOG("[RED] failed to create transform-feedback pipeline (vertex %s): %s", vs->key, error.localizedDescription.UTF8String);
        hm_put_id(&g_pso_fail, &key, NSNull.null); return nil;
    }
    hm_put_id(&g_pso, &key, pipeline);
    return pipeline;
}
static void tf_pend_add(uint64_t addr, uint64_t size) {
    pend_add(addr, size);
}
static void tf_draw(NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    __unsafe_unretained NvShader *vs = pr ? pr->sh[0] : nil;
    if (!S.tf_on) { PD("Z %llu draw skipped: rasterizer discard", g_stat_pass + !g_enc); return; }
    if (!vs || !vs->tf_fn) { RED_ONCE(100, "transform feedback vertex shader %s has no capture entry point", vs ? vs->key : "-"); return; }
    if (!dw || di || dw->ind || dw->mdi_cnt || dw->instances > 1) {
        RED_ONCE(101, "unsupported indexed, indirect or multi-instance transform-feedback draw (vertex %s, indexed %d indirect %#llx mdi %#llx instances %d prim %d)",
                 vs->key, di != NULL, (unsigned long long)(dw ? dw->ind : 0), (unsigned long long)(dw ? dw->mdi_cnt : 0), dw ? dw->instances : 0, dw ? dw->prim : -1); return;
    }
    if (dw->count <= 0) return;
    uint64_t gpu[4] = {0};
    for (int b = 0; b < 4; b++) {
        if (!vs->tf_stride[b]) continue;
        uint64_t size = (uint64_t)dw->count * vs->tf_stride[b];
        uint64_t at = S.tf[b].addr + S.tf_off[b]; void *buffer; uint64_t offset;
        if (!S.tf[b].addr || S.tf_off[b] + size > S.tf[b].size || !mtl_gpu_addr(at, size, &buffer, &offset, &gpu[b])) {
            RED_ONCE(102, "transform feedback buffer %d of vertex %s is missing, too small or outside a memory pool", b, vs->key); return;
        }
    }
    __unsafe_unretained id<MTLRenderPipelineState> pipeline = tf_pso(vs);
    if (!pipeline) return;
    rp_end();
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.renderTargetWidth = 1; pass.renderTargetHeight = 1; pass.defaultRasterSampleCount = 1;
    if (!g_bare_native) {
        pass.colorAttachments[0].texture = diag_c0_texture(1, 1, 1);
        if (!pass.colorAttachments[0].texture) { MLOG("[RED] Transform-feedback compatibility target allocation failed"); return; }
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionDontCare;
    }
    id<MTLRenderCommandEncoder> encoder = enc_r_begin_pass(pass);
    if (enc_label_on()) encoder.label = [NSString stringWithFormat:@"transform-feedback %.16s", vs->key];
    enc_r_pso(encoder, pipeline);
    bind_vbufs(encoder);
    args_dirty_all(); encode_args(encoder, vs, 0, 1);
    for (int b = 0; b < 4; b++) if (vs->tf_stride[b]) {
        uint64_t base = gpu[b] - (uint64_t)dw->first * vs->tf_stride[b];
        NSUInteger offset = 0; id<MTLBuffer> buffer = legacy_buffer_at(base, &offset);
        if (!buffer) { RED_ONCE(103, "transform feedback output address for buffer %d is not registered", b); enc_r_end_pass(encoder); args_dirty_all(); return; }
        legacy_bind_buffer(encoder, 0, TF_SLOT + b, buffer, offset);
        legacy_use_resource(encoder, buffer, MTLResourceUsageWrite, 0);
        uint64_t size = (uint64_t)dw->count * vs->tf_stride[b];
        tf_pend_add(S.tf[b].addr + S.tf_off[b], size); S.tf_off[b] += size;
    }
    enc_r_draw(encoder, MTLPrimitiveTypePoint, (NSUInteger)dw->first, (NSUInteger)dw->count);
    enc_r_end_pass(encoder);
    args_dirty_all(); g_enc_pso = nil; g_enc_dss = nil; g_ed.ok = 0;
    { static int once; if (!once++) MLOG("transform feedback: first character-skinning capture (vertex %s, prim %d, first %d, %d vertices, %d instances)", vs->key, dw->prim, dw->first, dw->count, dw->instances); }
}

// iOS 16 indirect draws must not consume unchecked guest records.  The record
// is often written by an earlier GPU pass, so reading it on the CPU is stale and
// waiting for a half-finished frame can turn a recoverable GPU fault into a
// permanently ignored queue.  Convert and bound it on the same GPU queue into
// a host-owned arena record instead.  The following render pass then consumes
// only that sanitized record, with no CPU wait and no mid-submission commit.
#define SAFE_INDIRECT_INDEX_CAP (3u * 16384u)
typedef struct {
    __unsafe_unretained id<MTLBuffer> args;
    NSUInteger args_off;
    __unsafe_unretained id<MTLBuffer> drv0;
    NSUInteger drv0_off;
    uint64_t drv0_gpu;
    __unsafe_unretained id<MTLBuffer> index;
    NSUInteger index_off, index_len;
    MTLIndexType index_type;
    __unsafe_unretained id<MTLFence> fence;
    int indexed;
    unsigned diag_id; uint64_t diag_frame;
} safe_indirect_draw;
static id<MTLFence> g_safe_indirect_fence;
#define SAFE_INDIRECT_CACHE_MAX 1024
typedef struct {
    int arrays;
    const uint8_t *command;
    int consumed;
    p_draw dw;
    p_draw_idx di;
    safe_indirect_draw out;
} safe_indirect_ent;
static safe_indirect_ent g_safe_indirect_cache[SAFE_INDIRECT_CACHE_MAX];
static int g_safe_indirect_n;
static uint64_t g_safe_indirect_epoch = ~0ull, g_safe_indirect_sub;
static uint64_t g_safe_indirect_batches, g_safe_indirect_batched, g_safe_indirect_splits;

static void quad_ib_make(void) {
    if (g_quad_ib) return;
    uint32_t n = 65536; uint32_t *ix = malloc((size_t)n * 6 * 4);
    for (uint32_t q = 0; q < n; q++) { uint32_t b = q * 4, v[6] = {b, b + 1, b + 2, b, b + 2, b + 3}; memcpy(ix + q * 6, v, 24); }
    g_quad_ib = [g_dev newBufferWithBytes:ix length:(size_t)n * 24 options:MTLResourceStorageModeShared];
    g_quad_gpu = legacy_buffer_gpu(g_quad_ib); res_add(g_quad_ib); free(ix);
}
static void fan_ib_make(void) {
    if (g_fan_ib) return;
    uint32_t n = 65536; uint32_t *ix = malloc((size_t)n * 3 * 4);
    for (uint32_t i = 0; i < n; i++) { uint32_t v[3] = {0, i + 1, i + 2}; memcpy(ix + i * 3, v, 12); }
    g_fan_ib = [g_dev newBufferWithBytes:ix length:(size_t)n * 12 options:MTLResourceStorageModeShared];
    g_fan_gpu = legacy_buffer_gpu(g_fan_ib); res_add(g_fan_ib); free(ix);
}

static const char k_safe_indirect_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct P { uint kind, indexCap, ordinal, hasCount, prim, indexSize, outCap, pad; };\n"
"kernel void safe_args(device const uint *r [[buffer(0)]], device const uint *mc [[buffer(1)]], device uint *o [[buffer(2)]], constant P &p [[buffer(3)]], device uint *drv [[buffer(6)]]) {\n"
"  bool live = !p.hasCount || p.ordinal < mc[0]; uint n = live ? r[0] : 0, inst = live ? r[1] : 0;\n"
"  if (inst > (1u << 20)) { n = 0; inst = 0; }\n"
"  drv[0x644/4] = p.indexSize ? r[4] : r[3];\n"
"  if (p.kind == 0) { n = min(n, 1u << 27); o[0]=n; o[1]=inst; o[2]=r[2]; o[3]=r[3]; return; }\n"
"  if (p.kind == 1) { uint first=r[2]; n = first < p.indexCap ? min(n, p.indexCap-first) : 0; o[0]=n; o[1]=inst; o[2]=first; o[3]=r[3]; o[4]=r[4]; return; }\n"
"  uint maxn = p.prim == 7 ? p.outCap / 6 * 4 : p.outCap / 3 + 2; n=min(n,maxn);\n"
"  uint c = p.prim == 7 ? n / 4 * 6 : n >= 3 ? (n - 2) * 3 : 0;\n"
"  o[0]=min(c,p.outCap); o[1]=inst; o[2]=0; o[3]=r[2]; o[4]=r[3];\n"
"}\n"
"kernel void safe_expand(device const uint *r [[buffer(0)]], device const uint *mc [[buffer(1)]], device uint *a [[buffer(2)]], constant P &p [[buffer(3)]], device const uchar *ib [[buffer(4)]], device uint *out [[buffer(5)]], device uint *drv [[buffer(6)]], uint i [[thread_position_in_grid]]) {\n"
"  bool live = !p.hasCount || p.ordinal < mc[0]; uint first=r[2], avail=first < p.indexCap ? p.indexCap-first : 0;\n"
"  uint n=live ? min(r[0],avail) : 0, inst=live ? r[1] : 0; if (inst > (1u << 20)) { n=0; inst=0; }\n"
"  uint maxn=p.prim==7 ? p.outCap/6*4 : p.prim==6 ? p.outCap/3+2 : p.outCap; n=min(n,maxn);\n"
"  uint m=min(p.prim == 7 ? n/4*6 : p.prim == 6 ? (n >= 3 ? (n-2)*3 : 0) : n,p.outCap);\n"
"  if (i==0) { a[0]=m; a[1]=inst; a[2]=0; a[3]=r[3]; a[4]=r[4]; drv[0x644/4]=p.indexSize ? r[4] : r[3]; } if (i>=m) return;\n"
"  uint t=i/3,c=i%3,q=p.prim==7 ? t/2*4+(c ? c+(t&1) : 0) : p.prim==6 ? (c ? t+c : 0) : i, x=first+q;\n"
"  out[i]=p.indexSize==1 ? uint(ib[x]) : p.indexSize==2 ? uint(((device const ushort*)ib)[x]) : ((device const uint*)ib)[x];\n"
"}\n"
"kernel void snapshot_args(device const uint *r [[buffer(0)]], device const uint *mc [[buffer(1)]], device uint *o [[buffer(2)]], constant P &p [[buffer(3)]]) {\n"
"  o[0]=r[0]; o[1]=r[1]; o[2]=r[2]; o[3]=r[3]; o[4]=p.indexSize ? r[4] : 0; o[5]=p.hasCount ? mc[0] : 0xffffffffu; o[6]=p.ordinal; o[7]=p.kind;\n"
"}\n";

static int safe_indirect_same(const safe_indirect_ent *e, const p_draw *dw, const p_draw_idx *di) {
    if (e->arrays != (dw != NULL)) return 0;
    if (dw) return e->dw.prim == dw->prim && e->dw.ind == dw->ind && e->dw.mdi_cnt == dw->mdi_cnt && e->dw.mdi_i == dw->mdi_i;
    return e->di.prim == di->prim && e->di.itype == di->itype && e->di.ibuf == di->ibuf && e->di.ind == di->ind
        && e->di.mdi_cnt == di->mdi_cnt && e->di.mdi_i == di->mdi_i;
}
static int safe_indirect_find(const p_draw *dw, const p_draw_idx *di, const uint8_t *command) {
    for (int i = 0; i < g_safe_indirect_n; i++) {
        safe_indirect_ent *e = &g_safe_indirect_cache[i];
        if (!e->consumed && e->command == command && safe_indirect_same(e, dw, di)) return i;
    }
    return -1;
}
static int safe_indirect_take(const p_draw *dw, const p_draw_idx *di, const uint8_t *command) {
    int i = safe_indirect_find(dw, di, command);
    if (i >= 0 || g_safe_indirect_n >= SAFE_INDIRECT_CACHE_MAX) return i;
    i = g_safe_indirect_n++;
    safe_indirect_ent *e = &g_safe_indirect_cache[i]; memset(e, 0, sizeof *e); e->arrays = dw != NULL; e->command = command;
    if (dw) e->dw = *dw; else e->di = *di;
    return i;
}

static int safe_indirect_encode(safe_indirect_ent *entry, id<MTLComputeCommandEncoder> ce,
                                id<MTLComputePipelineState> ps_args, id<MTLComputePipelineState> ps_expand,
                                id<MTLComputePipelineState> ps_snapshot) {
    const p_draw *dw = entry->arrays ? &entry->dw : NULL;
    const p_draw_idx *di = entry->arrays ? NULL : &entry->di;
    safe_indirect_draw *out = &entry->out;
    uint64_t ind = dw ? dw->ind : di->ind, cnt = dw ? dw->mdi_cnt : di->mdi_cnt;
    void *rv = NULL, *cv = NULL; uint64_t ro = 0, co = 0;
    NSUInteger rec_size = dw ? 16 : 20;
    if (!mtl_addr(ind, rec_size, &rv, &ro)) { RED_ONCE(89, "indirect record address %#llx is outside memory pools", (unsigned long long)ind); return 0; }
    if (cnt) { if (!mtl_addr(cnt, 4, &cv, &co)) { RED_ONCE(93, "indirect-count address %#llx is outside memory pools", (unsigned long long)cnt); return 0; } }
    else { cv = rv; co = ro; }

    typedef struct { uint32_t kind, index_cap, ordinal, has_count, prim, index_size, out_cap, pad; } safe_param;
    safe_param p = {.ordinal = (uint32_t)(dw ? dw->mdi_i : di->mdi_i), .has_count = cnt != 0,
                    .prim = (uint32_t)(dw ? dw->prim : di->prim), .out_cap = SAFE_INDIRECT_INDEX_CAP};
    id<MTLBuffer> ab; size_t ao = arena_alloc(32, &ab);
    id<MTLBuffer> db; size_t doff = arena_alloc(DRV_SIZE, &db);
    memset((uint8_t *)db.contents + doff, 0, DRV_SIZE);
    out->args = ab; out->args_off = ao; out->index = nil; out->index_off = out->index_len = 0; out->indexed = 0;
    out->drv0 = db; out->drv0_off = doff; out->drv0_gpu = legacy_buffer_gpu(db) + doff;
    if (!g_safe_indirect_fence) g_safe_indirect_fence = [g_dev newFence];
    out->fence = g_safe_indirect_fence;

    int special = di && (di->itype == 0 || di->prim == 6 || di->prim == 7);
    void *iv = NULL; uint64_t io = 0; NSUInteger isz = di ? (di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4) : 0;
    if (di) {
        if (!mtl_addr(di->ibuf, 1, &iv, &io)) { RED_ONCE(22, "indirect index buffer is outside memory pools"); return 0; }
        p.index_cap = (uint32_t)MIN(((__bridge id<MTLBuffer>)iv).length - io, (NSUInteger)UINT32_MAX) / (uint32_t)isz;
        p.index_size = (uint32_t)isz;
    }
    if (dw && (dw->prim == 6 || dw->prim == 7)) {
        if (dw->prim == 7) quad_ib_make(); else fan_ib_make();
        out->indexed = 1; out->index = dw->prim == 7 ? g_quad_ib : g_fan_ib; out->index_type = MTLIndexTypeUInt32;
        out->index_len = out->index.length; p.kind = 2; p.out_cap = (uint32_t)(out->index_len / 4);
    } else if (di && !special) {
        out->indexed = 1; out->index = (__bridge id<MTLBuffer>)iv; out->index_off = (NSUInteger)io;
        out->index_len = out->index.length - out->index_off; out->index_type = di->itype == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32; p.kind = 1;
    } else if (!di) p.kind = 0;

    [ce setBuffer:(__bridge id<MTLBuffer>)rv offset:(NSUInteger)ro atIndex:0];
    [ce setBuffer:(__bridge id<MTLBuffer>)cv offset:(NSUInteger)co atIndex:1];
    id<MTLBuffer> snapshot = nil; size_t snapshot_off = 0;
    if (ps_snapshot && geometry_diag_frame() && g_commit_blocks && g_geom_diag.indirect < 512) {
        snapshot_off = arena_alloc(32, &snapshot);
        out->diag_id = (unsigned)++g_geom_diag.indirect; out->diag_frame = g_geom_diag.frame;
        [ce setBuffer:snapshot offset:snapshot_off atIndex:2]; [ce setBytes:&p length:sizeof p atIndex:3];
        legacy_use_resource(ce, (__bridge id<MTLBuffer>)rv, MTLResourceUsageRead, 1);
        legacy_use_resource(ce, (__bridge id<MTLBuffer>)cv, MTLResourceUsageRead, 1);
        legacy_use_resource(ce, snapshot, MTLResourceUsageWrite, 1);
        enc_c_pso(ce, ps_snapshot); enc_c_dispatch(ce, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1));
    }
    [ce setBuffer:ab offset:ao atIndex:2]; [ce setBytes:&p length:sizeof p atIndex:3]; [ce setBuffer:db offset:doff atIndex:6];
    legacy_use_resource(ce, (__bridge id<MTLBuffer>)rv, MTLResourceUsageRead, 1);
    legacy_use_resource(ce, (__bridge id<MTLBuffer>)cv, MTLResourceUsageRead, 1);
    legacy_use_resource(ce, ab, MTLResourceUsageWrite, 1);
    legacy_use_resource(ce, db, MTLResourceUsageWrite, 1);
    if (special) {
        id<MTLBuffer> xb; size_t xo = arena_alloc((size_t)SAFE_INDIRECT_INDEX_CAP * 4, &xb);
        out->indexed = 1; out->index = xb; out->index_off = xo; out->index_len = (size_t)SAFE_INDIRECT_INDEX_CAP * 4; out->index_type = MTLIndexTypeUInt32;
        [ce setBuffer:(__bridge id<MTLBuffer>)iv offset:(NSUInteger)io atIndex:4]; [ce setBuffer:xb offset:xo atIndex:5];
        legacy_use_resource(ce, (__bridge id<MTLBuffer>)iv, MTLResourceUsageRead, 1); legacy_use_resource(ce, xb, MTLResourceUsageWrite, 1);
        enc_c_pso(ce, ps_expand); enc_c_dispatch(ce, MTLSizeMake(SAFE_INDIRECT_INDEX_CAP / 64, 1, 1), MTLSizeMake(64, 1, 1));
    } else { enc_c_pso(ce, ps_args); enc_c_dispatch(ce, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1)); }
    static _Atomic uint32_t diag_n;
    uint32_t diag_i = atomic_fetch_add_explicit(&diag_n, 1, memory_order_relaxed);
    if (diag_i < 128 && g_commit_blocks) {
        id<MTLBuffer> keep = ab, keep_drv = db; NSUInteger keep_off = ao, keep_drv_off = doff; uint64_t keep_ind = ind, keep_cnt = cnt;
        uint32_t keep_ordinal = p.ordinal, keep_kind = p.kind, keep_cap = p.index_cap;
        [g_commit_blocks addObject:^{
            const uint32_t *v = (const uint32_t *)((const uint8_t *)keep.contents + keep_off);
            const uint32_t *dv = (const uint32_t *)((const uint8_t *)keep_drv.contents + keep_drv_off);
            MLOG("safe-indirect result #%u kind=%u source=%#llx multi=%#llx[%u] cap=%u -> %u,%u,%u,%d,%u drvBaseInstance=%u",
                 diag_i, keep_kind, (unsigned long long)keep_ind, (unsigned long long)keep_cnt, keep_ordinal, keep_cap,
                 v[0], v[1], v[2], (int32_t)v[3], keep_kind ? v[4] : 0, dv[0x644 / 4]);
        }];
    }
    if (snapshot) {
        id<MTLBuffer> raw_keep = snapshot, args_keep = ab, drv_keep = db;
        size_t raw_offset = snapshot_off, args_offset = ao, drv_offset = doff;
        uint64_t frame = out->diag_frame, source = ind, count_source = cnt;
        unsigned record = out->diag_id; safe_param params = p;
        [g_commit_blocks addObject:^{
            const uint32_t *r = (const uint32_t *)((const uint8_t *)raw_keep.contents + raw_offset);
            const uint32_t *v = (const uint32_t *)((const uint8_t *)args_keep.contents + args_offset);
            const uint32_t *drv = (const uint32_t *)((const uint8_t *)drv_keep.contents + drv_offset);
            MLOG("[geometry-indirect] frame=%llu rec=%u source=%#llx count_source=%#llx ordinal=%u kind=%u prim=%u cap=%u raw=%u,%u,%u,%d,%u multi_count=%u result=%u,%u,%u,%d,%u driver_base=%u",
                 (unsigned long long)frame, record, (unsigned long long)source, (unsigned long long)count_source,
                 params.ordinal, params.kind, params.prim, params.index_cap, r[0], r[1], r[2], (int32_t)r[3], r[4], r[5],
                 v[0], v[1], v[2], (int32_t)v[3], params.kind ? v[4] : 0, drv[0x644 / 4]);
        }];
    }
    return 1;
}

static int safe_indirect_prepare(const p_draw *dw, const p_draw_idx *di, safe_indirect_draw *out) {
    uint64_t ind = dw ? dw->ind : di->ind;
    if (!ind) return 1;
    static id<MTLComputePipelineState> ps_args, ps_expand, ps_snapshot; static int failed;
    if (failed) return 0;
    if (!ps_args) {
        NSError *error = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_safe_indirect_msl) options:nil error:&error];
        id<MTLFunction> fa = [lib newFunctionWithName:@"safe_args"], fe = [lib newFunctionWithName:@"safe_expand"];
        ps_args = fa ? [g_dev newComputePipelineStateWithFunction:fa error:&error] : nil;
        ps_expand = fe ? [g_dev newComputePipelineStateWithFunction:fe error:&error] : nil;
        if (geometry_diag_file()) {
            id<MTLFunction> fs = [lib newFunctionWithName:@"snapshot_args"];
            ps_snapshot = fs ? [g_dev newComputePipelineStateWithFunction:fs error:&error] : nil;
            if (!ps_snapshot) MLOG("[geometry] indirect snapshot pipeline failed: %s", error.localizedDescription.UTF8String);
        }
        if (!ps_args || !ps_expand) { failed = 1; MLOG("[RED] safe indirect conversion pipeline failed: %s", error.localizedDescription.UTF8String); return 0; }
    }
    if (g_safe_indirect_epoch != g_gpu_write_epoch || g_safe_indirect_sub != g_sub_serial) {
        g_safe_indirect_n = 0; g_safe_indirect_epoch = g_gpu_write_epoch; g_safe_indirect_sub = g_sub_serial;
    }
    // Equal guest records do not imply equal driver state: shadow and main
    // passes bind different SSBO tables. Each command occurrence owns its c0,
    // and consuming it prevents a later draw from overwriting in-flight state.
    int hit = safe_indirect_find(dw, di, g_exec_next);
    if (hit >= 0) { g_safe_indirect_cache[hit].consumed = 1; *out = g_safe_indirect_cache[hit].out; return out->args != nil; }
    if (g_safe_indirect_n > SAFE_INDIRECT_CACHE_MAX - 64) g_safe_indirect_n = 0;
    int first = g_safe_indirect_n, current = safe_indirect_take(dw, di, g_exec_next);
    if (current < 0) return 0;
    if (g_exec_next && g_ret_n <= 8) {
        fz_cursor cur = {.p = g_exec_next, .ret_n = g_ret_n}; memcpy(cur.ret, g_ret, sizeof(*g_ret) * (size_t)g_ret_n);
        const uint8_t *q; op_hdr h; const void *a;
        for (int guard = 0; guard < 200000 && g_safe_indirect_n < SAFE_INDIRECT_CACHE_MAX && fz_next(&cur, &q, &h, &a); guard++) {
            if (h.op == OP_DISPATCH || h.op == OP_COPY_B2B || h.op == OP_UPDATE_UB || h.op == OP_CLEAR_BUF || h.op == OP_COPY_T2B) break;
            p_draw dt; p_draw_idx it;
            const p_draw *d = h.op == OP_DRAW ? draw_rec(a, h.size, &dt) : NULL;
            const p_draw_idx *x = h.op == OP_DRAW_IDX ? draw_idx_rec(a, h.size, &it) : NULL;
            if (d && d->ind) safe_indirect_take(d, NULL, q + h.size);
            else if (x && x->ind) safe_indirect_take(NULL, x, q + h.size);
        }
    }
    if (g_enc) { g_safe_indirect_splits++; rp_end(); }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTLComputeCommandEncoder> ce = enc_c_begin(NULL);
    int made = 0;
    for (int i = first; i < g_safe_indirect_n; i++) made += safe_indirect_encode(&g_safe_indirect_cache[i], ce, ps_args, ps_expand, ps_snapshot);
    if (g_safe_indirect_fence) [ce updateFence:g_safe_indirect_fence];
    enc_c_end(ce); args_dirty_all(); g_enc_pso = nil; g_enc_dss = nil; g_ed.ok = 0;
    g_safe_indirect_batches++; g_safe_indirect_batched += (uint64_t)(g_safe_indirect_n - first);
    if (g_safe_indirect_batches == 1 || !(g_safe_indirect_batches % 1800))
        MLOG("iOS16 batched safe indirect: batch %llu converted %d records (cumulative %llu), render-pass splits %llu; explicit compute-to-indirect fence, no CPU wait or mid-frame commit",
             (unsigned long long)g_safe_indirect_batches, made, (unsigned long long)g_safe_indirect_batched, (unsigned long long)g_safe_indirect_splits);
    if (!g_safe_indirect_cache[current].out.args) return 0;
    g_safe_indirect_cache[current].consumed = 1;
    *out = g_safe_indirect_cache[current].out;
    { static int once; if (!once++) MLOG("indirect c0 ownership: one immutable driver block per command occurrence; GPU baseInstance preserved across split commits"); }
    return 1;
}

static void draw(const p_draw *dw, const p_draw_idx *di) {
    if (!S.prog) { RED_ONCE(18, "绘制前没有绑定程序"); PD("Z %llu 绘制跳过：没有绑定程序", g_stat_pass + !g_enc); return; }
    safe_indirect_draw sid = {0};
    if ((dw && dw->ind) || (di && di->ind)) if (!safe_indirect_prepare(dw, di, &sid)) return;
    g_draw_binst = (uint32_t)(dw ? dw->base_instance : di ? di->base_instance : 0);
    int driver_changed = sid.drv0 != g_indirect_drv0_buf;
    g_indirect_drv0_buf = sid.drv0; g_indirect_drv0_off = sid.drv0_off; g_indirect_drv0_gpu = sid.drv0_gpu;
    if (sid.drv0 || driver_changed) g_args_dirty[0] = 1;   // also invalidate when returning to a direct draw
    __unsafe_unretained NvProg *pr = prog_get(S.prog);
    NSUInteger slices = !g_slice_replaying && !S.rast_discard ? slice_target_count() : 1;
    if (slices > 1) {
        int unsupported = !pr || !pr->sh[0] || (dw && dw->ind) || (di && di->ind) || g_defer.active || g_fz.active;
        if (pr) for (int st = 2; st < 6; st++) unsupported |= pr->sh[st] != nil;
        if (pr && pr->sh[1]) for (int i = 0; i < pr->sh[1]->rf.nb; i++) {
            const nvn_rbind *b = &pr->sh[1]->rf.b[i];
            unsupported |= b->kind == NVN_RB_IMG || b->kind == NVN_RB_IMGA || (b->kind == NVN_RB_SBUFP && b->rw);
        }
        if (unsupported) {
            MLOG("[RED] Per-slice draw unsupported: indirect, missing shader, writable resources or active geometry/merge path");
            return;
        }
        rp_end();
        typeof(R) saved = R; g_slice_replaying = 1;
        for (uint32_t slice = 0; slice < slices; slice++) {
            R = saved; slice_select(slice);
            if (pr->sh[0]->slice_variant || slice == 0) draw(dw, di);
            else { int pending = 0; for (int i = 0; i < 8; i++) pending |= R.clear_c[i]; if (pending) rp_begin_ex(1); }
            rp_flush();
        }
        R = saved; memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
        g_slice_replaying = 0; g_selected_slice = 0; args_dirty_all(); g_pso_dirty = 1;
        static int notes; if (notes++ < 8) MLOG("Per-slice draw: %lu depth planes, vs=%s fs=%s; original instance/baseInstance preserved", (unsigned long)slices, pr->sh[0]->key, pr->sh[1] ? pr->sh[1]->key : "-");
        return;
    }
    if (sid.diag_id) MLOG("[geometry-draw] phase=replay frame=%llu rec=%u cmd=%p vs=%s fs=%s cull=%u depth=%u/%u/%u stencil=%u scissor=%d,%d,%d,%d",
        (unsigned long long)sid.diag_frame, sid.diag_id, g_exec_next, pr && pr->sh[0] ? pr->sh[0]->key : "-",
        pr && pr->sh[1] ? pr->sh[1]->key : "-", S.poly.cull, S.ds.dt, S.ds.dw, S.ds.df, S.ds.st,
        R.sc.x, R.sc.y, R.sc.w, R.sc.h);
    if (S.rast_discard) { tf_draw(pr, dw, di); return; }
    if (g_nubd && g_enc && ubd_conflict(pr, dw, di)) { g_ubd_conflicts++; rp_end(); }   // land the deferred inline updates before this draw (see ubd_conflict)
    if (g_ruler_truth == 0) {   // Ruler negative control (DM_NVN_RULER_NEG=<fragment key>): in the mode under test drop this shader's draws, and the ruler must report non-equivalence
        static const char *neg; static int ng = -1; if (ng < 0) { neg = getenv("DM_NVN_RULER_NEG"); ng = neg && *neg; }
        if (ng && pr) for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (sh && sh->stage == 1 && !strncmp(sh->key, neg, strlen(neg))) { static int once; if (!once++) MLOG("DM_NVN_RULER_NEG：被测模式丢弃着色器 %s 的绘制", sh->key); return; } }
    }
    if (zskip_can_draw(pr)) {   // Zero-source skip: the output is always 0, writing 0 over 0, so this draw changes no result (see zskip_can_draw)
        g_zskip_count++; { static int once; if (!once++) MLOG("零源跳过：首例 通道 %llu", g_stat_pass + 1); }
        PD("N %llu 零源跳过", g_stat_pass + 1); return; }
    if (!g_defer_replaying) {
        if (g_defer.active && g_defer.in_p && !g_defer.at_q) { if (defer_take(dw, di)) return; }
    }
    // Draws that cannot draw anything (both faces culled, empty scissor) return before the render pass is opened: otherwise the render pass is already open and on TBDR the whole attachment is loaded and stored for nothing
    // (Mac character select page: 3 render passes are opened only because of such draws, all loaded, zero draws)
    // Diagnostic switch for measuring an upper bound DM_NVN_SKIP_FS=<fragment key>[,<fragment key>...] (none by default): drops all draws of these fragment shaders (the picture will be wrong; only used in real-device ABA to measure "how much GPU time this class of draws is worth in total")
    { static char sk[16][20]; static int nsk = -1;
      if (nsk < 0) { nsk = 0; for (const char *q = getenv("DM_NVN_SKIP_FS"); q && *q && nsk < 16; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) { size_t n = strcspn(q, ","); if (n > 16) n = 16; memcpy(sk[nsk], q, n); sk[nsk++][n] = 0; }
                     if (nsk) MLOG("DM_NVN_SKIP_FS：丢弃 %d 个片元着色器的绘制（诊断，画面不对）", nsk); }
      if (g_ub_n > 0 && pr->sh[1] && !g_defer_replaying && ub_skip(pr->sh[1]->key)) return;   // In-process alternating measurement: this phase drops this class
      if (nsk && pr->sh[1] && !g_defer_replaying) for (int i = 0; i < nsk; i++) if (!strncmp(pr->sh[1]->key, sk[i], strlen(sk[i]))) return;
      if (ms_draw(pr)) return; }
    // Similar diagnostic DM_NVN_SKIP_SMALL=<width>: drop all draws whose target width (by the attached level) is <= this value -- measures how much "dropping tiny render passes in bulk" is worth under the parallel pipeline (fixed per-pass overhead)
    { static int small = -1; if (small < 0) { const char *e = getenv("DM_NVN_SKIP_SMALL"); small = e ? atoi(e) : 0; if (small) MLOG("DM_NVN_SKIP_SMALL：丢弃目标宽度 ≤ %d 的绘制（诊断，画面不对）", small); }
      if (small > 0 && R.has_rt && !g_defer_replaying) { int k = R.rt.n && R.rt.color[0] ? 0 : R.rt.depth ? 8 : -1;
        if (k >= 0) { uint64_t s_; int lv, sl; rt_sub(&R.rt, k, &s_, &lv, &sl); const tex_t *t = k == 8 ? R.rt.depth : R.rt.color[0]; if ((int)MAX(t->d.width >> lv, 1u) <= small) return; } } }
    // Experimental switch DM_NVN_NOFXAA=1 (off by default; keep or drop decided by the visual ruler + looking at the picture on a real machine): skips the FXAA full-screen draw, and the later sharpening / upscale reads FXAA's input instead (the tone mapping result)
    { static int nofxaa = -1; static uint64_t fx_from, fx_to; if (nofxaa < 0) { const char *e = getenv("DM_NVN_NOFXAA"); nofxaa = e && !strcmp(e, "1"); if (nofxaa) MLOG("DM_NVN_NOFXAA=1：跳过 FXAA"); }
      if (nofxaa && tbdr_on() && !g_defer_replaying && pr->sh[1]) { NvShader *f = pr->sh[1];
        if (!strncmp(f->key, "ebcfcd1fa0a77dbc", 16) && !g_enc && R.rt.n == 1 && R.rt.color[0] && !R.clear_c[0]) {
            uint64_t h = 0; for (int i = 0; i < f->rf.nb && !h; i++) if (f->rf.b[i].kind == NVN_RB_TEX && f->rf.b[i].reg < NTEX) h = S.tex[1][f->rf.b[i].reg];
            if (h) { fx_from = R.rt.color[0]->serial; fx_to = h; PD("N %llu 跳过 FXAA：ser=%llu 的读者改读 ser=%llu", g_stat_pass, (unsigned long long)fx_from, (unsigned long long)h_ser(h)); return; } }
        else if (fx_from && !strncmp(f->key, "52aac1f2306b8fc5", 16)) { for (int r = 0; r < NTEX; r++) if (S.tex[1][r] && h_ser(S.tex[1][r]) == fx_from) S.tex[1][r] = fx_to; fx_from = 0; } } }
    { static uint64_t bloomq_frame; if (bloomq_frame != atomic_load(&g_cur_frame)) { bloomq_frame = atomic_load(&g_cur_frame); bloomq_reset(); }
      if (bloomq_enabled() && tbdr_on() && !g_defer_replaying && pr->sh[1]) {
        NvShader *f = pr->sh[1]; uint64_t tgt = 0; int lv, sl;
        if (!strncmp(f->key, "2938a5b71b8e21b2", 16)) {
            if (g_bloomq.state) bloomq_reset();
            if (bloomq_target(&tgt, &lv, &sl) && R.rt.color[0]) {
                uint64_t h = 0; const tex_t *st = NULL; int cbuf = 0;
                for (int i = 0; i < f->rf.nb; i++) { nvn_rbind *b = &f->rf.b[i];
                    if (b->kind == NVN_RB_TEX && b->reg < NTEX && !h) { h = S.tex[1][b->reg]; int tid = (int)(h & 0xfffff); if (tid > 0 && tid < g_ntreg && g_treg[tid].ok) st = &g_treg[tid].t; }
                    if (b->kind == NVN_RB_SAMPT && b->reg < NTEX) g_bloomq.samp_h = S.tex[1][b->reg] >> 20;
                    if (b->kind == NVN_RB_SAMP && b->reg < 128) g_bloomq.samp_h = S.samp[1][b->reg] >> 20;
                    if (b->kind == NVN_RB_CBUF && !cbuf) cbuf = ubo_float(1, (uint16_t)(b->reg << 8), &g_bloomq.threshold);   // c7[0].x
                }
                if (h && st && cbuf && f->rf.c1_len >= sizeof(float) * 2) {
                    float c1[2]; memcpy(c1, f->rf.c1, sizeof c1); g_bloomq.s_tex = st; g_bloomq.s_h = h; g_bloomq.s_ser = st->serial; g_bloomq.b0_ser = tgt; g_bloomq.coeff_g = c1[0]; g_bloomq.coeff_b = c1[1]; g_bloomq.state = 1;
                    { static int once; if (!once++) MLOG("泛光 1/4 起点：生效（S ser=%llu B0 ser=%llu）", (unsigned long long)g_bloomq.s_ser, (unsigned long long)g_bloomq.b0_ser); } PD("A %llu 泛光 1/4 起点 S=%llu B0=%llu", g_stat_pass, (unsigned long long)g_bloomq.s_ser, (unsigned long long)g_bloomq.b0_ser); R.clear_c[0] = 0;   // The whole B0 level is no longer needed: drop the pending clear too, otherwise a clear-only render pass would be opened just for it
                    return;
                }
            }
        } else if (g_bloomq.state == 1 && !strncmp(f->key, "019ce2f82edbda84", 16)) {
            uint64_t src = 0; for (int i = 0; i < f->rf.nb; i++) if (f->rf.b[i].kind == NVN_RB_TEX && f->rf.b[i].reg < NTEX) { src = h_ser(S.tex[1][f->rf.b[i].reg]); break; }
            if (src == g_bloomq.b0_ser && bloomq_target(&tgt, &lv, &sl) && bloomq_capture()) { R.clear_c[0] = 0; g_bloomq.b1_ser = tgt; g_bloomq.state = 2; PD("B %llu 泛光 1/4 原生截取 B0=%llu B1=%llu", g_stat_pass, (unsigned long long)g_bloomq.b0_ser, (unsigned long long)g_bloomq.b1_ser); return; }
            RED_ONCE(62, "泛光 1/4 起点：截取已跳过但原生截取没画成，本帧缺泛光"); bloomq_reset();
        } else if (g_bloomq.state == 2 && !strncmp(f->key, "019ce2f82edbda84", 16)) {
            for (int i = 0; i < f->rf.nb; i++) if (f->rf.b[i].kind == NVN_RB_TEX && f->rf.b[i].reg < NTEX && h_ser(S.tex[1][f->rf.b[i].reg]) == g_bloomq.b1_ser) { g_bloomq.b1_h = S.tex[1][f->rf.b[i].reg]; break; }
        } else if (g_bloomq.state == 2 && !strncmp(f->key, "cc264583f2000c81", 16)) {
            if (bloomq_target(&tgt, &lv, &sl) && tgt == g_bloomq.b0_ser) { g_bloomq.state = 3; PD("C %llu 泛光 1/4 跳过 B0 上采样", g_stat_pass); return; }
        } else if (g_bloomq.state == 3 && !strncmp(f->key, "2e2d6700a2569795", 16)) {
            if (g_bloomq.b1_h) { for (int r = 0; r < NTEX; r++) if (S.tex[1][r] && h_ser(S.tex[1][r]) == g_bloomq.b0_ser) S.tex[1][r] = g_bloomq.b1_h; PD("D %llu 泛光 1/4 叠加改读 B1", g_stat_pass); g_bloomq_last_b0 = g_bloomq.b0_ser; g_bloomq_last_b1h = g_bloomq.b1_h; g_bloomq_last_b1s = g_bloomq.b1_ser; bloomq_reset(); }
            else { RED_ONCE(63, "泛光 1/4 状态缺少 B1 句柄"); bloomq_reset(); }
        } else if (!g_bloomq.state && g_bloomq_last_b1h && h_ser(g_bloomq_last_b1h) == g_bloomq_last_b1s && !strncmp(f->key, "2e2d6700a2569795", 16)) {   // The B1 handle still points to the same texture (not recognized after a rebuild on resolution change)
            // The bloom overlay is reached without the bloom chain having run this frame (the extrapolated frame of runtime extrapolation skipped it: outside the region the input 469 is the previous frame's LDR, so it cannot run): redirect the read to B1 as last time, overlaying the previous frame's bloom.
            // Without the redirect it would read B0 -- with bloom 1/4 on, B0's upsample is always skipped and never written, so what is read is uninitialized video memory (on M4 Pro the units in the region turn entirely green; elsewhere it is mostly 0, i.e. missing bloom)
            int hit = 0; for (int r = 0; r < NTEX; r++) if (S.tex[1][r] && h_ser(S.tex[1][r]) == g_bloomq_last_b0) { S.tex[1][r] = g_bloomq_last_b1h; hit = 1; }
            if (hit) { PD("D %llu 泛光链没跑：叠加沿用上一帧 B1", g_stat_pass); static uint64_t n; if (n++ % 600 == 0) MLOG("泛光 1/4：泛光链没跑的帧叠加沿用上一帧 B1（第 %llu 次）", (unsigned long long)n); }
        } else if (g_bloomq.state) { RED_ONCE(61, "泛光 1/4 起点：链中途出现别的绘制，本帧泛光不完整"); bloomq_reset(); }
      }
    }
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK) { PD("Z %llu 绘制跳过（开通道前）：两面全剔", g_stat_pass + !g_enc); return; }
    // Opt-in fault localization: give selected draws their own encoder labels.
    // Keep the original draw, resources, attachment load/store and query path.
    static const char *split_keys; static int split_init; static unsigned split_count;
    if (!split_init) { split_init = 1; split_keys = getenv("DM_NVN_DIAG_SPLIT_DRAW"); }
    if (split_keys && *split_keys && split_count < 2048 &&
        ((pr->sh[0] && sample_trace_key(split_keys, pr->sh[0]->key)) ||
         (pr->sh[1] && sample_trace_key(split_keys, pr->sh[1]->key)))) {
        split_count++; rp_end();
    }
    // The continued render pass may have been opened with fewer depth / stencil attachments (open modes 2 / 3 / 4): if the plane this draw needs is not attached, split before it (on reopen, attach per the new lookahead)
    if (g_rt_merged && g_enc_mode != 0 && (((S.ds.dt || S.ds.dw) && !g_pass.hd) || (S.ds.st && !g_pass.hs))) {
        static int once2; if (!once2++) MLOG("同目标续用：续用通道内绘制要的深度 / 模板平面没挂，在此切开（首例 通道 %llu）", g_stat_pass);
        PD("M %llu 续用通道内绘制要的深度 / 模板平面没挂，在此切开", g_stat_pass); rp_end(); }
    if (g_rt_range_active && (!rt_range_pr_ro(pr) || !S.ds.dt || S.ds.dw || S.ds.st)) rp_end();   // The external write visibility at the original encoder boundary is not removed
    if (g_rt_merged && draw_samples_rt(pr)) { static int once; if (!once++) MLOG("同目标续用：续用通道内绘制要采样附件，在此切开（首例 通道 %llu）", g_stat_pass);
        PD("M %llu 续用通道内绘制要采样附件，在此切开", g_stat_pass); rp_end(); }
    static id<MTLFunction> fz_fn;   // Specialized fragment function for this draw of the later pass in a merged render pass (nil = use the original version)
    if (g_fz.active && g_enc && g_fz.phase == 2 && !fz_p2_draw(pr, dw, di, g_fz.lmap2, &fz_fn)) {   // Cannot be proven: end the merged render pass (the earlier pass's result is stored as usual) and reopen with the later pass's original targets
        static int once3; if (!once3++) MLOG("可读 tile 通道合并：后一个通道的绘制证不出，退回分开（首例 通道 %llu）", g_stat_pass);
        g_fz_fallback++; rp_end(); }
    fb_prepare(pr);   // same-pass feedback sampling: copy the snapshot first (see fb_prepare)
    if (R.has_sc && !g_enc) {
        NSUInteger tw0 = 0, th0 = 0; rt_dims(&tw0, &th0);
        long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
        if (x1 > (long)tw0) x1 = tw0; if (y1 > (long)th0) y1 = th0;
        if (x1 <= x0 || y1 <= y0) { PD("Z %llu 绘制跳过（开通道前）：剪裁为空（%d,%d %dx%d，目标 %lux%lu）", g_stat_pass + 1, R.sc.x, R.sc.y, R.sc.w, R.sc.h, (unsigned long)tw0, (unsigned long)th0); return; }
    }
#ifdef NVN_RT_RANGE_TEST
    if (!g_enc) rr_begin();
#endif
    int defer_q_draw = !g_enc && !g_defer_replaying && g_defer_natq > 0;   // The draw that opens Q's render pass: after the render pass is opened as usual through hoist_try / fuse_try, replay P first and then draw this one
    if (g_frz_state) goto frz_plain;   // Full frame / frozen frame of world freeze: no hoisting, no deferral, no merging (the pass-skipping state machine must see every set-target)
    if (!g_enc && tbdr_on()) hoist_try();   // First hoist and execute the pure-clear render passes sandwiched between this render pass and its continuation (before rp_begin_ex, not occupying this render pass)
    // The deferral decision comes after hoist_try: hoisted pure-clear render passes are skipped in the lookahead, and only then can P's same-target continuation segment be recognized
    if (!g_enc && tbdr_on() && !defer_has_in_p() && !g_defer_replaying && !g_fz.active && !g_fz_checking && g_exec_next) { defer_try(dw, di); if (g_defer_cur->active && g_defer_cur->in_p) return; }
    if (!g_enc && tbdr_on() && !g_fz_checking) fuse_try();   // Readable tile render pass merge is on by default; when the structure / same-pixel proof does not hold it still falls back to a normal render pass, and it is not re-entered during the CHECK redraw
frz_plain:;
    int prim = dw ? dw->prim : di->prim;
    uint8_t topology = pso_topology_class(prim);
    if (g_pso_topology != topology) { g_pso_topology = topology; g_pso_dirty = 1; }
    if (!zero_image_prepare(pr, dw, di, g_defer.active || g_fz.active)) return;
    int need = g_enc || !tbdr_on() ? 3 : pass_ds_need(), mode = defer_q_draw ? 0 : need == 0 ? 2 : need == 2 ? 3 : need == 1 ? 4 : 0;   // 2 attach neither, 3 attach stencil only, 4 attach depth only
    int skip_d = mode != 0;
    if (!g_enc) { g_fd_dw = dw; g_fd_di = di; }
    id<MTLRenderCommandEncoder> en = rp_begin_ex(mode);
    g_fd_dw = NULL; g_fd_di = NULL;
    if (!en) { PD("Z %llu 绘制跳过：没开成渲染通道", g_stat_pass); return; }   // Z lines: why a draw was dropped at these early returns
    if (defer_q_draw) defer_replay();
    if (skip_d) { g_stat_ds_omit++; if (mode == 3) g_stat_s_only++; else if (mode == 4) g_stat_d_only++; }
    // Sampler-specialized fragments select their pipeline from the actual
    // argument-block snapshot, not a second read of mutable descriptor IDs.
    // fb_prepare has already supplied any feedback view. Bind this block once;
    // a registry update during compilation is handled on the next encode.
    int min_fs = min_reduction_candidate(pr->sh[1]);
    if (min_fs) { g_kd_force = kd_draw(pr, dw, di); encode_args(en, pr->sh[1], 1, 0); }
    // The pipeline depends only on (program, render pass targets, blend / color / write mask / multisample state, vertex layout): if these did not change, reuse the previous one
    static id<MTLRenderPipelineState> cur_ps; static NvProg *cur_pr; static uint64_t cur_ps_pass;
    if (g_fz.active && !g_defer_replaying) { g_pso_lmap = g_fz.phase == 1 ? g_fz.lmap1 : g_fz.lmap2; g_pso_fsfn = g_fz.phase == 1 ? nil : fz_fn; g_pso_fz = g_fz.phase == 1 ? (pso_fz_desc){0} : g_fz_desc; g_pso_dirty = 1; }   // Readable tile merge: fetch the pipeline by the physical -> logical mapping and the specialization
    if (g_pso_dirty || pr != cur_pr || cur_ps_pass != g_pass.serial || min_fs) { cur_ps = pso_get(pr, nil); cur_pr = pr; cur_ps_pass = g_pass.serial; g_pso_dirty = !cur_ps; }
    if (g_pso_lmap) { g_pso_lmap = NULL; g_pso_fsfn = nil; memset(&g_pso_fz, 0, sizeof g_pso_fz); g_pso_dirty = 1; }   // The next draw fetches again according to its own situation
    __unsafe_unretained id<MTLRenderPipelineState> ps = cur_ps;
    if (!ps) { PD("Z %llu 绘制跳过：没有渲染管线（顶点 %s 片元 %s）", g_stat_pass, pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[1] ? pr->sh[1]->key : "-"); return; }
    enc_r_pso(en, ps); g_enc_pso = ps;
    static id<MTLDepthStencilState> cur_ds; static uint64_t cur_ds_pass;
    if (g_dss_dirty || cur_ds_pass != g_pass.serial) { cur_ds = dss_get(g_pass.hd, g_pass.hs); cur_ds_pass = g_pass.serial; g_dss_dirty = 0; }
    enc_r_dss(en, cur_ds); g_enc_dss = cur_ds;
    { uint32_t sr = g_rt_ovr && g_pass.hs ? (g_rt_ovr == 3 ? 0x40 : 0) : S.st_ref[0];   // enc_r_stencil_ref sends the front value only
      if (!(g_ed.ok & ED_SREF) || g_ed.sref != sr) { enc_r_stencil_ref(en, sr, sr); g_ed.sref = sr; g_ed.ok |= ED_SREF; } }
    // Viewport / scissor (clamped within the target size)
    NSUInteger tw = g_pass.tw, th = g_pass.th;
    // When the UI rectangle does not touch the top edge of the picture (iPad true full screen: the UI is laid out at 16:9 at the bottom of the picture, see dm/host/dm_uiscreen.c), UI draws are shifted down as a whole: a vertical offset is added to the projection matrix (ui_mat_patch), and the scissor moves down with it.
    // A UI draw = 4 vertices, 2 attributes, 20-byte vertices, in a pass of its own and composited at end of frame (measured: startup loading, character select, character creation, entering-game loading, in-game, open panel, and full-screen map never mix in other draws).
    // When the matrix cannot be read this time (there are still unlanded writes in this submit), fall back to moving the viewport: same position, except that widgets lifted to the top get clipped by the viewport's top edge this one time.
    // A scissor starting from the top edge (y <= 0) does not have its top edge moved down: when the host lifts top-anchored widgets (minimap, area name etc.) back to the real top of the picture, they are drawn above the UI rectangle and must not be clipped.
    // Exception: the untextured block that exactly covers the whole UI rectangle (the dimming when a panel opens, the black curtain of a transition) is changed to cover the whole picture -- the viewport is scaled vertically by 1/h and not moved down, otherwise the 3D outside the UI rectangle is left with a light/dark dividing line.
    // When nobody registered the entry or the UI rectangle is the whole picture (iPhone, Mac default), vo = so = 0, vs = 1, the matrix is not changed, and what follows is word-for-word the same as before.
    long vo = 0, so = 0; double vs = 1, moff = 0;
    if (g_ui_rect_fn && dw && dw->count == 4 && S.nattr == 2 && S.stream[0].stride == 20) { double uy = 0, uh = 1; g_ui_rect_fn(&uy, &uh);
        if (uh > 0 && uh < 1) { if (ui_cover_all(uh)) vs = 1 / uh; else if (uy > 0) { so = lround(uy * th); if (ui_mat_ok()) moff = 2 * uy; else { vo = so; static int n; if (n++ < 4) MLOG("界面下移：投影矩阵这次读不到，退回挪视口（通道 %llu）", g_stat_pass); } } } }
    if (moff != g_ui_moff) { g_ui_moff = moff; g_args_dirty[0] = 1; }
    { MTLViewport v = S.has_vp ? (MTLViewport){S.vp[0][0], S.vp[0][1] + vo, S.vp[0][2], S.vp[0][3] * vs, S.dr[0], S.dr[1]} : (MTLViewport){0, vo, tw, th * vs, 0, 1};
      if (atomic_load_explicit(&g_origin_ll, memory_order_relaxed)) { v.originY += v.height; v.height = -v.height; }   // lower-left origin (see g_origin_ll)
      if (!(g_ed.ok & ED_VP) || memcmp(&g_ed.vp, &v, sizeof v)) { enc_r_viewport(en, v); g_ed.vp = v; g_ed.ok |= ED_VP; } }
    MTLScissorRect sc = {0, 0, tw, th};
    if (R.has_sc) {
        long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y <= 0 ? 0 : R.sc.y + so, x1 = (long)R.sc.x + R.sc.w, y1 = vs != 1 ? (long)th : (long)R.sc.y + R.sc.h + so;
        if (x1 > (long)tw) x1 = tw; if (y1 > (long)th) y1 = th;
        if (x1 <= x0 || y1 <= y0) { PD("Z %llu 绘制跳过：剪裁为空（%d,%d %dx%d，目标 %lux%lu）", g_stat_pass, R.sc.x, R.sc.y, R.sc.w, R.sc.h, (unsigned long)tw, (unsigned long)th); return; }
        sc = (MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)};
    }
    if (!(g_ed.ok & ED_SC) || memcmp(&g_ed.sc, &sc, sizeof sc)) { enc_r_scissor(en, sc); g_ed.sc = sc; g_ed.ok |= ED_SC; }
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK) { PD("Z %llu 绘制跳过：两面全剔", g_stat_pass); return; }
    static int diag_no_cull = -1;
    if (diag_no_cull < 0) { const char *e = getenv("DM_NVN_CULL_NONE"); diag_no_cull = e && !strcmp(e, "1"); if (diag_no_cull) MLOG("DM_NVN_CULL_NONE=1: disable triangle culling"); }
    enc_r_cull(en, diag_no_cull ? MTLCullModeNone : S.poly.cull == NVN_FACE_FRONT ? MTLCullModeFront : S.poly.cull == NVN_FACE_BACK ? MTLCullModeBack : MTLCullModeNone);
    enc_r_winding(en, (S.poly.front == NVN_FRONT_FACE_CCW) != atomic_load_explicit(&g_origin_ll, memory_order_relaxed) ? MTLWindingCounterClockwise : MTLWindingClockwise);   // the y flip of lower-left mode reverses the winding
    enc_r_fill(en, S.poly.mode == NVN_POLYGON_MODE_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill);
    if (S.poly.offset_en & 4) enc_r_depth_bias(en, S.po.units, S.po.factor, S.po.clamp); else enc_r_depth_bias(en, 0, 0, 0);
    enc_r_depth_clip(en, !S.depth_clamp);   // NVN depth clamp enabled means Metal depth clipping is disabled.
    enc_r_blend_color(en, S.blend_color[0], S.blend_color[1], S.blend_color[2], S.blend_color[3]);
    bind_vbufs(en);
    if (!min_fs) g_kd_force = kd_draw(pr, dw, di);
    g_sample_trace_dw = dw; g_sample_trace_di = di;
    if (pr->sh[0]) encode_args(en, pr->sh[0], 0, 1);
    if (pr->sh[1] && !min_fs) encode_args(en, pr->sh[1], 1, 0);
    if (pr->sh[0] && pr->sh[0]->slice_variant) [en setVertexBytes:&g_selected_slice length:sizeof g_selected_slice atIndex:30];
    if (pr->sh[1] && pr->sh[1]->slice_variant) [en setFragmentBytes:&g_selected_slice length:sizeof g_selected_slice atIndex:30];
    if (pr->sh[1] && pr->sh[1]->zero_image_variant) [en setFragmentBytes:g_zero_image_layout length:sizeof g_zero_image_layout atIndex:29];
    // Parameter binding captures the actual resources under the registry lock; if the version changed between the eligibility check and the binding, split and retry before any GPU draw.
    if (g_rt_range_active && (g_pass.reg_gen != g_reg_gen || g_pass.ser_gen != g_ser_gen || g_pass.range_dirty != nvn_dirty_now())) { rp_end(); draw(dw, di); return; }
    MTLPrimitiveType mp;
    switch (prim) {
        case 0: mp = MTLPrimitiveTypePoint; break; case 1: mp = MTLPrimitiveTypeLine; break; case 3: mp = MTLPrimitiveTypeLineStrip; break;
        case 4: case 6: case 7: mp = MTLPrimitiveTypeTriangle; break; case 5: mp = MTLPrimitiveTypeTriangleStrip; break;   // 6 triangle fan, 7 quads: Metal has neither, expanded into triangle lists
        default: RED_ONCE(19, "图元类型 %d 未实现", prim); return;
    }
    if (g_vis_counting) {   // Occlusion query counting: a new slot for this render pass (also switch to a new slot after a report / reset)
        if (!g_enc_has_vis) RED_ONCE(34, "遮挡查询计数开始时所在通道没挂可见性缓冲（首次清零发生在通道中途），该通道的采样不计");
        else {
            if (g_vis_cur < 0 || g_vis_cur_pass != g_pass.serial) {
                if (g_vis_n >= VIS_SLOTS) RED_ONCE(35, "可见性槽用尽");
                else { g_vis_cur = (int)g_vis_n++; g_vis_cur_pass = g_pass.serial;
                    if (g_nvs == g_vscap) { g_vscap = g_vscap ? g_vscap * 2 : 64; g_vs = realloc(g_vs, g_vscap * sizeof *g_vs); }
                    g_vs[g_nvs++] = (vis_slot){(void *)CFBridgingRetain(g_vis), (uint32_t)g_vis_cur}; }
            }
            if (g_vis_cur >= 0 && g_enc_vis != g_vis_cur + 1) { enc_r_visibility(en, MTLVisibilityResultModeCounting, (NSUInteger)g_vis_cur * 8); g_enc_vis = g_vis_cur + 1; }
        }
    }
    g_stat_draw++;
    pixel_history_draw(pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[1] ? pr->sh[1]->key : "-");
    { static int tg; if (!tg) { tg = 1; const char *e = getenv("DM_TEST_GHOST"); float v[8];   // Test switch DM_TEST_GHOST=px,py,gx,gy,gw,gh,cx,cy (normalized; press point, grid, center of the drag-follow image): turns on the drag-follow image directly without going through the front end (for Mac host screenshot verification)
        if (e && sscanf(e, "%f,%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8) mtl_ui_ghost(1, v[0], v[1], &v[2], v[6], v[7]); } }
    if (dw && atomic_load(&g_ghost_on)) ghost_note(dw);
    if (enc_label_on() && !g_el_draws++ && pr->sh[1]) { memcpy(g_el_key, pr->sh[1]->key, 8); g_el_key[8] = 0; }
    if (passdump()) {   // Characteristics of a render pass's first draw (to judge whether the load can be omitted: full screen, no blending, writes all channels, does not depend on depth); with DM_NVN_PASSDUMP_ALL every draw is recorded
        static uint64_t last_pass = ~0ull; static int all = -1; if (all < 0) all = getenv("DM_NVN_PASSDUMP_ALL") != NULL;
        if (all || g_kd_force) PD("d %llu bv=%d bi=%d first=%d vs=%s fs=%s vc=%d inst=%d ind=%#llx mdi=%#llx mi=%d blend=%#x b0=%#llx dt=%d dw=%d df=%d cull=%d st=%d sf=%d,%d sop=%d,%d,%d/%d,%d,%d smask=%#x,%#x/%#x,%#x sref=%#x,%#x ib=%#llx it=%d cmask=%#x b1=%#llx b2=%#llx b3=%#llx", g_stat_pass, di ? di->base_vertex : 0, di ? di->base_instance : 0, dw ? dw->first : 0, pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[1] ? pr->sh[1]->key : "-",
                    dw ? dw->count : di->count, dw ? dw->instances : di->instances,
                    (unsigned long long)(dw ? dw->ind : di->ind), (unsigned long long)(dw ? dw->mdi_cnt : di->mdi_cnt), dw ? dw->mdi_i : di->mdi_i,
                    S.color.blend_en, (unsigned long long)*(uint64_t *)&S.blend[0], (int)S.ds.dt, (int)S.ds.dw, (int)S.ds.df, (int)S.poly.cull,
                    (int)S.ds.st, (int)S.ds.sf_f, (int)S.ds.sf_b, (int)S.ds.fail_f, (int)S.ds.zfail_f, (int)S.ds.zpass_f,
                    (int)S.ds.fail_b, (int)S.ds.zfail_b, (int)S.ds.zpass_b, S.st_vmask[0], S.st_vmask[1], S.st_wmask[0], S.st_wmask[1], S.st_ref[0], S.st_ref[1],
                    di ? (unsigned long long)di->ibuf : 0ull, di ? di->itype : 0, S.chmask.mask, (unsigned long long)*(uint64_t *)&S.blend[1], (unsigned long long)*(uint64_t *)&S.blend[2], (unsigned long long)*(uint64_t *)&S.blend[3]);
        if (all && pr->sh[0]) {   // Content fingerprint of each constant buffer of the vertex stage (to judge whether a draw is completely identical across frames: used by the shadow cache etc.); when unreadable (a write pending execution in this submit) record P
            char ub[400] = {0}; size_t o = 0; NvShader *v = pr->sh[0];
            for (int i = 0; i < v->rf.nb && o < sizeof ub - 40; i++) if (v->rf.b[i].kind == NVN_RB_CBUF && v->rf.b[i].reg < 16) {
                const p_bind_buf *u = &S.ubo[0][v->rf.b[i].reg]; void *mb; uint64_t mo; const uint8_t *base; uint64_t h = 1469598103934665603ull;
                if (!u->addr || !u->size) { o += snprintf(ub + o, sizeof ub - o, " r%d=-", v->rf.b[i].reg); continue; }
                if (pend_hits(u->addr, u->size) || !mtl_addr(u->addr, u->size, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) { o += snprintf(ub + o, sizeof ub - o, " r%d=P/%llu", v->rf.b[i].reg, (unsigned long long)u->size); continue; }
                uint64_t h64 = 0; for (uint64_t q = 0; q < u->size; q++) { h = (h ^ base[mo + q]) * 1099511628211ull; if (q == 63) h64 = h; }   // h64: the first 64 bytes (world matrix vec0–3 of the object buffer)
                { static uint64_t hp = ~0ull; static int hn; if (hp != g_stat_pass) { hp = g_stat_pass; hn = 0; }   // For the first 6 draws of each render pass, additionally print buffers of <= 608 bytes in hex (to compare byte by byte which fields changed across frames)
                  if (hn < 12 && u->size <= 608 && v->rf.b[i].reg == 3) { hn++; char hx[1300]; for (uint64_t q = 0; q < u->size; q++) snprintf(hx + 2 * q, 3, "%02x", base[mo + q]); PD("x %llu r%d %s", g_stat_pass, v->rf.b[i].reg, hx); } }
                o += snprintf(ub + o, sizeof ub - o, " r%d=%016llx/%llu/%016llx", v->rf.b[i].reg, (unsigned long long)h, (unsigned long long)u->size, (unsigned long long)h64); }
            PD("u %llu%s", g_stat_pass, ub); }
        if (all) { char vb[1600]; size_t o = 0; o += snprintf(vb + o, sizeof vb - o, "nattr=%d", S.nattr);
            for (int i = 0; i < S.nattr && i < 16 && o < sizeof vb - 100; i++) { int st = S.attr[i].stream; if (!S.attr[i].fmt) continue;   // only attributes with a format
                o += snprintf(vb + o, sizeof vb - o, " a%d{fmt=%#x off=%d s=%d stride=%u div=%u vb=%#llx/%llu}", i, S.attr[i].fmt, S.attr[i].offset, st,
                              st < 16 ? S.stream[st].stride : 0, st < 16 ? S.stream[st].divisor : 0, st < 16 ? (unsigned long long)S.vbuf[st].address : 0ull, st < 16 ? (unsigned long long)S.vbuf[st].size : 0ull); }
            PD("v %llu %s", g_stat_pass, vb);
            // DM_NVN_CBDUMP + DM_NVN_CBDUMP_KEY=vertex shader key: also save that draw's vertex streams and indices (F<frame>_p<pass>_vb<stream>_<address>.bin / _ib_<address>.bin)
            const char *cbd = getenv("DM_NVN_CBDUMP"), *cbk = getenv("DM_NVN_CBDUMP_KEY");
            if (cbd && cbk && pr->sh[0] && !strcmp(pr->sh[0]->key, cbk)) {
                char fp[1024];
                for (int st = 0; st < 16; st++) if (S.vbuf[st].address && S.vbuf[st].size) {
                    snprintf(fp, sizeof fp, "%s/F%llu_p%llu_vb%d_%llx.bin", cbd, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass, st, (unsigned long long)S.vbuf[st].address);
                    FILE *f = fopen(fp, "wb"); if (f) { fwrite((const void *)S.vbuf[st].address, 1, S.vbuf[st].size, f); fclose(f); } }
                if (di && di->ibuf) {
                    snprintf(fp, sizeof fp, "%s/F%llu_p%llu_ib%d_%llx.bin", cbd, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass, di->itype, (unsigned long long)di->ibuf);
                    FILE *f = fopen(fp, "wb"); if (f) { fwrite((const void *)di->ibuf, 1, (size_t)di->count << (di->itype == 2 ? 2 : di->itype), f); fclose(f); } }
            } }
        if (last_pass != g_stat_pass) {
            last_pass = g_stat_pass;
            PD("D %llu prim=%d vc=%d inst=%d blend=%#x wm=%#x dt=%d dw=%d df=%d st=%d sc=%lu,%lu,%lu,%lu tgt=%lux%lu vp=%.0f,%.0f,%.0f,%.0f", g_stat_pass, prim,
               dw ? dw->count : di->count, dw ? dw->instances : di->instances, S.color.blend_en, S.chmask.mask, (int)S.ds.dt, (int)S.ds.dw, (int)S.ds.df, (int)S.ds.st,
               (unsigned long)sc.x, (unsigned long)sc.y, (unsigned long)sc.width, (unsigned long)sc.height, (unsigned long)tw, (unsigned long)th,
               S.has_vp ? S.vp[0][0] : 0, S.has_vp ? S.vp[0][1] : 0, S.has_vp ? S.vp[0][2] : (float)tw, S.has_vp ? S.vp[0][3] : (float)th);
        }
    }
    g_kd_force = 0;
    if (rt_range_on() && !rt_range_pr_ro(pr)) g_pass.range_external=1;
    rt_range_note_depth();
#ifdef NVN_RT_RANGE_TEST
    rr_snap(dw,di);
#endif
    udc_note_draw(pr);
    if (enc_label_on()) {
        NSString *mark = [NSString stringWithFormat:@"d%llu v=%.16s f=%.16s n=%d",
                          (unsigned long long)g_stat_draw,
                          pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[1] ? pr->sh[1]->key : "-",
                          dw ? dw->count : di->count];
        [en insertDebugSignpost:mark];
    }
    // A draw with 0 primitives (the game does send count=0) draws no pixels, so skip encoding; Metal requires the count to be non-zero, otherwise the validation layer aborts on the spot (measured on the camp / wilderness / combat pages)
    if ((dw && dw->ind) || (di && di->ind)) {
        if (sid.fence) [en waitForFence:sid.fence beforeStages:MTLRenderStageVertex];
        if (sid.indexed) enc_r_draw_indexed_indirect(en, mp, sid.index_type, sid.index, sid.index_off, sid.index_len, sid.args, sid.args_off);
        else enc_r_draw_indirect(en, mp, sid.args, sid.args_off);
    } else if (dw && prim == 7) {   // Quads: shared indices 0,1,2,0,2,3
        quad_ib_make();
        if (dw->count >= 4) enc_r_draw_indexed(en, mp, (dw->count / 4) * 6, MTLIndexTypeUInt32, g_quad_gpu, g_quad_ib.length, dw->instances > 0 ? dw->instances : 1, dw->first, dw->base_instance);
    } else if (dw && prim == 6) {   // triangle fans (13 Sentinels): shared indices (0, i, i+1), first as the base vertex
        fan_ib_make();
        if (dw->count >= 3) enc_r_draw_indexed(en, mp, (dw->count - 2) * 3, MTLIndexTypeUInt32, g_fan_gpu, g_fan_ib.length, dw->instances > 0 ? dw->instances : 1, dw->first, dw->base_instance);
    } else if (dw) {
        if (dw->count && (dw->instances > 1 || dw->base_instance)) enc_r_draw_instanced(en, mp, dw->first, dw->count, dw->instances > 0 ? dw->instances : 1, dw->base_instance);
        else if (dw->count) enc_r_draw(en, mp, dw->first, dw->count);
    } else {
        if (prim == 6 || prim == 7) {
                                        // indexed triangle fans / quads (13 Sentinels): at replay the indices are read from guest memory and expanded into a 32-bit triangle list in this frame's arena (as for 8-bit indices)
            int isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4;
            uint32_t nt = prim == 7 ? (uint32_t)(di->count / 4) * 2 : di->count >= 3 ? (uint32_t)di->count - 2 : 0;
            if (!nt) return;
            void *mb; uint64_t mo; const uint8_t *src;
            if (!mtl_addr(di->ibuf, (uint64_t)di->count * isz, &mb, &mo) || !(src = [(__bridge id<MTLBuffer>)mb contents])) { RED_ONCE(22, "索引缓冲不在内存池里"); return; }
            if (pend_hits(di->ibuf, (uint64_t)di->count * isz)) RED_ONCE(73, "三角扇 / 四边形索引缓冲本提交里有 GPU 侧写入：按当前内容展开");
            src += mo;
            #define IX(i) (isz == 1 ? (uint32_t)src[i] : isz == 2 ? (uint32_t)((const uint16_t *)src)[i] : ((const uint32_t *)src)[i])
            id<MTLBuffer> ab; size_t off = arena_alloc(((size_t)nt * 12 + 15) & ~(size_t)15, &ab);
            uint32_t *d = (uint32_t *)((uint8_t *)ab.contents + off);
            for (uint32_t t = 0; t < nt; t++) {
                if (prim == 6) { d[3 * t] = IX(0); d[3 * t + 1] = IX(t + 1); d[3 * t + 2] = IX(t + 2); }
                else { uint32_t q = (t >> 1) * 4, h = t & 1; d[3 * t] = IX(q); d[3 * t + 1] = IX(q + 1 + h); d[3 * t + 2] = IX(q + 2 + h); }
            }
            #undef IX
            use_rec("buf", di->ibuf, (uint64_t)di->count * isz, 0, NULL);
            enc_r_draw_indexed(en, mp, nt * 3, MTLIndexTypeUInt32, g_cur_arena->gpu + off, (NSUInteger)nt * 12, di->instances ? di->instances : 1, di->base_vertex, di->base_instance);
            if (g_rt_range_pending) { PD("j rt_level_range pass=%llu vc=%u", g_stat_pass, (unsigned)di->count); g_rt_range_pending = 0; }
            return;
        }
        if (di->itype == 0) {
                                // 8-bit indices: Metal only has 16 / 32-bit. At replay they are read from guest memory and widened to 16-bit in this frame's arena (SMBW's full-screen passes and the final copy into the window all use 8-bit indices; without this the whole screen is black)
            if (!di->count) return;
            void *mb; uint64_t mo; const uint8_t *src;
            if (!mtl_addr(di->ibuf, di->count, &mb, &mo) || !(src = [(__bridge id<MTLBuffer>)mb contents])) { RED_ONCE(22, "索引缓冲不在内存池里"); return; }
            if (pend_hits(di->ibuf, di->count)) RED_ONCE(48, "8 位索引缓冲本提交里有 GPU 侧写入：按当前内容展开");
            id<MTLBuffer> ab; size_t off = arena_alloc(((size_t)di->count * 2 + 15) & ~(size_t)15, &ab);
            uint16_t *d = (uint16_t *)((uint8_t *)ab.contents + off); src += mo;
            for (int32_t i = 0; i < di->count; i++) d[i] = src[i];
            use_rec("buf", di->ibuf, (uint64_t)di->count, 0, NULL);
            enc_r_draw_indexed(en, mp, di->count, MTLIndexTypeUInt16, g_cur_arena->gpu + off, (NSUInteger)di->count * 2, di->instances ? di->instances : 1, di->base_vertex, di->base_instance);
            if (g_rt_range_pending) { PD("j rt_level_range pass=%llu vc=%u", g_stat_pass, (unsigned)di->count); g_rt_range_pending = 0; }
            return;
        }
        void *mb; uint64_t mo;
        uint64_t gpu;
        if (!mtl_gpu_addr(di->ibuf, 1, &mb, &mo, &gpu)) { RED_ONCE(22, "索引缓冲不在内存池里"); return; }
        use_rec("buf", di->ibuf, (uint64_t)di->count * (di->itype == 1 ? 2 : 4), 0, NULL);
        if (di->count) enc_r_draw_indexed(en, mp, di->count, di->itype == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32, gpu, ((__bridge id<MTLBuffer>)mb).length - mo,
                di->instances ? di->instances : 1, di->base_vertex, di->base_instance);
    }
    if (g_rt_range_pending) { PD("j rt_level_range pass=%llu vc=%u", g_stat_pass, (unsigned)(dw ? dw->count : di->count)); g_rt_range_pending=0; }
}
// Function identity is part of the cache key: a sampler-specialized companion
// must never replace a pipeline for the shader's ordinary sampler bindings.
typedef struct { const void *shader, *function; } compute_key;
static nvn_hmap g_cps = {.klen = sizeof(compute_key)};
static void warm_claim(void) {
    if (!atomic_exchange_explicit(&g_warm_ready, 0, memory_order_acq_rel)) return;
    os_unfair_lock_lock(&g_warm_lock);
    NSArray<WarmItem *> *items = [g_warm_items copy];
    [g_warm_items removeAllObjects];
    os_unfair_lock_unlock(&g_warm_lock);
    for (WarmItem *item in items) {
        if (item->kind == 0) {
            if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
            if (!g_shcache[item->key]) { g_shcache[item->key] = item->shader; g_warm_claim_shaders++; }
        } else if (item->kind == 1) {
            if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
            NvShader *vs = g_shcache[item->vs_key], *fs = item->fs_key.length ? g_shcache[item->fs_key] : nil;
            if (!vs) { vs = item->vs; g_shcache[item->vs_key] = vs; }
            if (item->fs_key.length && !fs) { fs = item->fs; g_shcache[item->fs_key] = fs; }
            item->pso.vs = (__bridge void *)vs; item->pso.fs = (__bridge void *)fs;
            if (!hm_get_id(&g_pso, &item->pso)) { hm_put_id(&g_pso, &item->pso, item->object); g_warm_claim_rps++; }
        } else {
            NvShader *cs = g_shcache[item->key];
            if (!cs) { cs = item->shader; if (!g_shcache) g_shcache = [NSMutableDictionary dictionary]; g_shcache[item->key] = cs; }
            compute_key key = {(__bridge const void *)cs, (__bridge const void *)cs->fn};
            if (!hm_get_id(&g_cps, &key)) { hm_put_id(&g_cps, &key, item->object); g_warm_claim_cps++; }
        }
    }
}
// ---------- 3D picture brightening ----------
// Tone mapping (fragment a1b1aab66933fa8f) = PQ-encode the composited HDR x0.008, then look up a 32^3 color lookup table (recomputed per region every frame by compute shader 552f45c348a61b86, read only by tone mapping).
// Multiplying HDR by exposure k first is equivalent to replacing the table with L'(u) = L(PQ(k * PQ^-1(u))): after the dispatch in which the game finishes computing the table, make a copy and rewrite it in place accordingly (one small 32^3 dispatch); the table is recomputed every frame, so it does not get brighter with each multiplication.
// Only the 3D world is touched: UI, text and the health orbs are composited after tone mapping; highlights are compressed by PQ and do not blur into a sheet of white as a final whole-screen multiply would. The normal / readable tile merge / freeze paths all just read this table, so it takes effect in one place.
// k is given every frame by the host's dm_bright_k() (dm/host/dm_bright.c: 1 out of game, 4 for maze-type areas, 4 where the original game's picture does not follow day and night, the rest derived from the original game's ambient brightness, with smooth transitions); no rewrite when k ~= 1.
__attribute__((weak)) float dm_bright_k(void) { return 1.0f; }   // The host's dm_bright.c provides the strong definition; no brightening when graphics unit tests do not link the host
static const char k_lut_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"constant float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;\n"   // Bit-identical to the PQ constants in the tone mapping shader
"static float3 pq(float3 v) { float3 p = pow(abs(v), m1); return pow((c1 + c2 * p) / (1.0 + c3 * p), m2); }\n"
"static float3 pq_inv(float3 u) { float3 q = pow(u, 1.0 / m2); return pow(max(q - c1, 0.0) / max(c2 - c3 * q, 1e-6), 1.0 / m1); }\n"
"kernel void lutk(texture3d<float> src [[texture(0)]], texture3d<float, access::write> dst [[texture(1)]], constant float &k [[buffer(0)]], uint3 g [[thread_position_in_grid]]) {\n"
"  uint3 n = uint3(dst.get_width(), dst.get_height(), dst.get_depth()); if (any(g >= n)) return;\n"
"  constexpr sampler ls(filter::linear, address::clamp_to_edge, coord::normalized);\n"
"  float3 u = (float3(g) + 0.5) / float3(n); float3 u2 = clamp(pq(k * pq_inv(u)), 0.0, 1.0);\n"
"  float4 b = src.sample(ls, u2), o = src.read(g);\n"   // b: output after brightening by exposure k; o: this cell in the original game
"  float s = smoothstep(0.65, 1.0, dot(b.rgb, float3(0.2126, 0.7152, 0.0722)));\n"   // Highlight protection: after brightening, the closer to white, the more it is pulled back to the original value (snow and skylight do not blur into a sheet of white); in dark areas s = 0 and brightening proceeds as usual
"  dst.write(float4(max(mix(b.rgb, o.rgb, s), o.rgb), b.a), g); }\n";
static void bright_lut(NvShader *cs) {
    static int failed; static id<MTLComputePipelineState> ps; static id<MTLTexture> tmp; static id<MTLBuffer> kb[3]; static int ki;
    if (failed || strncmp(cs->key, "552f45c348a61b86", 16)) return;
    float k = dm_bright_k(); if (!(k > 1.005f)) return;   // k ~= 1 (out of game / noon): the table stays as in the original game
    if (k > 4.0f) k = 4.0f;
    int tid = 0; for (int i = 0; i < cs->rf.nb && !tid; i++) if (cs->rf.b[i].kind == NVN_RB_IMG && cs->rf.b[i].reg < 32) tid = (int)(S.img[5][cs->rf.b[i].reg] & 0xfffff);
    pthread_mutex_lock(&g_rmu); id<MTLTexture> lut = tid ? tex_by_id(tid) : nil; pthread_mutex_unlock(&g_rmu);
    if (!lut || lut.textureType != MTLTextureType3D || !(lut.usage & MTLTextureUsageShaderWrite)) { static int n; if (!n++) MLOG("【红】3D 画面加亮：调色表认不出（贴图号 %d），不加亮", tid); return; }
    if (!ps) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_lut_msl) options:nil error:&e]; id<MTLFunction> f = lib ? [lib newFunctionWithName:@"lutk"] : nil;
        ps = f ? nvn_cached_compute(g_dev, f, &e) : nil;
        for (int i = 0; i < 3; i++) { kb[i] = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; if (kb[i]) res_add(kb[i]); }
        if (!ps || !kb[0] || !kb[1] || !kb[2]) { failed = 1; MLOG("【红】3D 画面加亮：计算管线建不出 %s（退回原版亮度）", e.localizedDescription.UTF8String); return; }
        MLOG("3D 画面加亮：建好（调色表 %lux%lux%lu，曝光每帧由 dm_bright_k 给）", (unsigned long)lut.width, (unsigned long)lut.height, (unsigned long)lut.depth); }
    if (!tmp || tmp.width != lut.width || tmp.height != lut.height || tmp.depth != lut.depth || tmp.pixelFormat != lut.pixelFormat) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor new]; d.textureType = MTLTextureType3D; d.pixelFormat = lut.pixelFormat; d.width = lut.width; d.height = lut.height; d.depth = lut.depth;
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate; if (tmp) defer_obj_release(tmp); tmp = [g_dev newTextureWithDescriptor:d]; if (!tmp) { failed = 1; return; } res_add(tmp); }
    id<MTLBuffer> b0 = kb[ki]; ki = (ki + 1) % 3; *(float *)b0.contents = k;   // Three copies in rotation: in-flight frames are still reading the previous copy
    id<MTLBlitCommandEncoder> b = blit(); enc_b_copy_texture(b, lut, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(lut.width, lut.height, lut.depth), tmp, 0, 0, MTLOriginMake(0, 0, 0)); enc_b_end(g_blit); g_blit = nil;
    id<MTLComputeCommandEncoder> ce = (id<MTLComputeCommandEncoder>)enc_c_begin(NULL);  [ce setTexture:tmp atIndex:0]; [ce setTexture:lut atIndex:1]; [ce setBuffer:b0 offset:0 atIndex:0];
    enc_c_pso(ce, ps); enc_c_dispatch(ce, MTLSizeMake((lut.width + 3) / 4, (lut.height + 3) / 4, (lut.depth + 3) / 4), MTLSizeMake(4, 4, 4)); enc_c_end(ce);
    args_dirty_all();
}
static id<MTLComputePipelineState> compute_pipeline(NvShader *cs, id<MTLFunction> fn) {
    compute_key k = {(__bridge const void *)cs, (__bridge const void *)fn};
    id<MTLComputePipelineState> ps = hm_get_id(&g_cps, &k);
    if (!ps) {
        NSError *e = nil; uint64_t bt = bp_now(); ps = nvn_cached_compute(g_dev, fn, &e);
        g_bp_pso_n++; g_bp_pso_ns += bp_now() - bt;
        if (!ps) { MLOG("[RED] Compute pipeline %s failed: %s", cs->key, e.localizedDescription.UTF8String); return nil; }
        if (fn == cs->fn) { pso_record("C\t%s\n", cs->key);
            if (cs->key[0]) pso_user_record("C\t%s\n", cs->key); }
        hm_put_id(&g_cps, &k, ps);
    }
    return ps;
}
static void dispatch(const p_dispatch *dp) {
    zskip_clear();
    if (!S.prog) { RED_ONCE(27, "compute dispatch has no bound program"); return; }
    NvShader *cs = prog_get(S.prog)->sh[5];
    if (!cs) { RED_ONCE(28, "dispatched program has no mapped compute shader"); return; }
    int min_cs = min_reduction_candidate(cs);
    id<MTLComputePipelineState> ps = min_cs ? nil : compute_pipeline(cs, cs->fn);
    if (!min_cs && !ps) return;
    if (cs->builtin) {   // Built-in compute kernel: n = uniform buffer 0, src = storage buffer 0, dst = storage buffer 1
        const p_bind_buf *u = &S.ubo[5][0], *a0 = &S.ssbo[5][0], *a1 = &S.ssbo[5][1];
        void *ub, *sb, *db; uint64_t uo, so, dof;
        if (!mtl_addr(u->addr, 4, &ub, &uo) || !mtl_addr(a0->addr, 8, &sb, &so) || !mtl_addr(a1->addr, 8, &db, &dof)) { RED_ONCE(30, "内嵌计算核 %s 的缓冲没绑全", cs->builtin); return; }
        if (!cs->logged++) {
            module_t *m = module_of((void *)dp->caller);
            MLOG("内嵌计算核 %s 首次派发：调用点 %s+%#llx，线程组 %dx%dx%d，元素数（CPU 侧当前值）%u", cs->builtin, m ? m->name : "?",
                 m ? (unsigned long long)(dp->caller - (uint64_t)m->base) : (unsigned long long)dp->caller, dp->x, dp->y, dp->z, *(uint32_t *)u->addr);
        }
        rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
        id<MTLComputeCommandEncoder> ce = pg_compute();
        enc_c_pso(ce, ps);
        enc_c_args(ce, legacy_buffer_gpu((__bridge id<MTLBuffer>)ub) + uo, 0); enc_c_args(ce, legacy_buffer_gpu((__bridge id<MTLBuffer>)sb) + so, 1); enc_c_args(ce, legacy_buffer_gpu((__bridge id<MTLBuffer>)db) + dof, 2);   // Built-in compute kernel: cold path, take the addresses directly
        enc_c_dispatch(ce, MTLSizeMake(dp->x, dp->y, dp->z), MTLSizeMake(32, 1, 1));
        enc_c_end(ce);
        pend_add(a1->addr, a1->size ? a1->size : 1);
        return;
    }
    const uint16_t *tg = cs->rf.tg;
    if (!tg[0]) { RED_ONCE(29, "计算着色器缺线程组大小反射"); return; }
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTLComputeCommandEncoder> ce = pg_compute();
    if (enc_label_on()) ce.label = [NSString stringWithFormat:@"compute %.16s %dx%dx%d", cs->key, dp->x, dp->y, dp->z];
    if (!min_cs) enc_c_pso(ce, ps);
    g_dispatch_dim[0] = (uint32_t)dp->x; g_dispatch_dim[1] = (uint32_t)dp->y; g_dispatch_dim[2] = (uint32_t)dp->z;   // read back by the driver segment c0 (encode_args_)
    encode_args(ce, cs, 5, 0);
    if (min_cs) {
        id<MTLFunction> fn = min_reduction_function(cs) ?: cs->fn;
        ps = compute_pipeline(cs, fn);
        if (!ps && fn != cs->fn) { cs->min_reduction_pso_failed = 1;
            MLOG("Min-reduction %s pipeline rejected; disabling its companion and retrying the ordinary pipeline", cs->key);
            ps = compute_pipeline(cs, cs->fn); }
        if (!ps) { enc_c_end(ce); return; }
        enc_c_pso(ce, ps);
    }
    enc_c_dispatch(ce, MTLSizeMake(dp->x, dp->y, dp->z), MTLSizeMake(tg[0], tg[1], tg[2]));
    enc_c_end(ce);
    bright_lut(cs);   // 3D picture brightening: rewrite after the game finishes computing the color lookup table
}
// GPU: texture -> buffer (row stride taken from SetCopyRowStride, layer stride from SetCopyImageStride, 0 = tightly packed)
static void copy_t2b(const p_copy_b2t *c) {
    za_diag_op("copy-texture-to-buffer", c->tex);
    if (c->tex) xv_check(c->tex->serial, c->has_view && c->view.set_levels ? c->view.base_level : 0, c->has_view && c->view.set_layers ? c->view.min_layer : 0, "贴图读缓冲");
    id<MTLTexture> t = tex_mtl(c->tex);
    if (!t) return;
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    unsigned level = c->has_view && c->view.set_levels ? c->view.base_level : 0, layer0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((c->r.width + bw - 1) / bw) * bpb;
    size_t rows = (c->r.height + bh - 1) / bh, img = S.copy_img_stride ? (size_t)S.copy_img_stride : row * rows;   // per layer: SetCopyImageStride, else tightly packed
    int is3d = c->tex->d.target == 2, nz = c->r.depth ? c->r.depth : 1;
    void *mb; uint64_t mo;
    if (!mtl_addr(c->src, img * nz, &mb, &mo)) { RED_ONCE(31, "贴图→缓冲拷贝的目标不在内存池里"); return; }
    if (nvn_fmt_bc((uint32_t)c->tex->d.format) && !is_native_bc(t.pixelFormat)) {
        RED_ONCE(52, "a BC texture stored decompressed or as ASTC cannot be copied back as original BC blocks; clearing the destination");
        id<MTLBlitCommandEncoder> zero_blit = blit();
        [zero_blit fillBuffer:(__bridge id<MTLBuffer>)mb range:NSMakeRange(mo, img * nz) value:0];
        return;
    }
    id<MTLBlitCommandEncoder> b = blit();
    if (is3d)
        enc_b_copy_texture_buffer(b, t, 0, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, c->r.zoffset), MTLSizeMake(c->r.width, c->r.height, nz), (__bridge id<MTLBuffer>)mb, mo, row, img);
    else
        for (int z = 0; z < nz; z++)
            enc_b_copy_texture_buffer(b, t, layer0 + c->r.zoffset + z, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, 0), MTLSizeMake(c->r.width, c->r.height, 1), (__bridge id<MTLBuffer>)mb, mo + z * img, row, img);
}
// CPU texture read (ReadTexels): for packaged textures the pool memory is the ground truth, de-layout and read directly; the rest are read back from the MTLTexture by blit (the caller has already waited for the GPU per NVN rules)
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    xv_check(t->serial, level, layer0, "CPU读回");
    PD("U cpu_read ser=%llu lv%u sl%u xyz=%d,%d,%d size=%d,%d,%d dst=%p row=%lu img=%lu", t->serial, level, layer0, r->xoffset, r->yoffset, r->zoffset, r->width, r->height, r->depth, dst, (unsigned long)row, (unsigned long)img);
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (r->height + bh - 1) / bh, x0 = (size_t)(r->xoffset / bw) * bpb, y0 = r->yoffset / bh;
    int nz = r->depth ? r->depth : 1, is3d = t->d.target == 2;
    if (t->has_layout && t->mem) {
        uint32_t lw = MAX(t->d.width >> level, 1u), lh = MAX(t->d.height >> level, 1u), lz = is3d ? MAX(t->d.depth >> level, 1u) : 1;
        size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh;
        uint8_t *lin = malloc(lp * lr * lz);
        for (int z = 0; z < nz; z++) {
            unsigned layer = is3d ? 0 : layer0 + r->zoffset + z;
            if ((is3d && z == 0) || !is3d) if (nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) { RED_ONCE(32, "ReadTexels 解排布失败"); free(lin); return; }
            const uint8_t *src = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0);
            for (size_t y = 0; y < hb; y++) memcpy(dst + z * img + y * row, src + (y0 + y) * lp + x0, wb);
        }
        free(lin);
        return;
    }
    // This runs on a game thread outside submission replay. Bound the Metal
    // command objects' autorelease lifetime because that thread may live forever.
    @autoreleasepool {
    id<MTLTexture> mt = tex_mtl(t);
    if (!mt) { memset(dst, 0, img * nz); return; }
    if (nvn_fmt_bc((uint32_t)t->d.format) && !is_native_bc(mt.pixelFormat)) { RED_ONCE(51, "a GPU-written BC texture stored decompressed or as ASTC cannot be read back as original BC blocks; returning zeroes"); memset(dst, 0, img * nz); return; }
    mtl_finish_gpu();
    id<MTLBuffer> buf = [g_dev newBufferWithLength:wb * hb * nz options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = enc_b_begin(cb);
    for (int z = 0; z < nz; z++)
        [b copyFromTexture:mt sourceSlice:is3d ? 0 : layer0 + r->zoffset + z sourceLevel:level sourceOrigin:MTLOriginMake(r->xoffset, r->yoffset, is3d ? r->zoffset + z : 0)
                sourceSize:MTLSizeMake(r->width, r->height, 1) toBuffer:buf destinationOffset:z * wb * hb destinationBytesPerRow:wb destinationBytesPerImage:wb * hb];
    enc_b_end(b); [cb commit]; [cb waitUntilCompleted];
    for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memcpy(dst + z * img + y * row, (uint8_t *)buf.contents + z * wb * hb + y * wb, wb);
    }
}
static void copy_b2t(const p_copy_b2t *c) {
    za_diag_op("copy-buffer-to-texture", c->tex);
    if (c->tex) zskip_del(c->tex->serial);
    id<MTLTexture> t = tex_mtl(c->tex);
    PD("C ser=%llu", c->tex->serial);
    if (!t) return;
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    unsigned level = c->has_view && c->view.set_levels ? c->view.base_level : 0, layer0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((c->r.width + bw - 1) / bw) * bpb;
    size_t rows = (c->r.height + bh - 1) / bh, img = S.copy_img_stride ? (size_t)S.copy_img_stride : row * rows;   // per layer: SetCopyImageStride, else tightly packed
    int is3d = c->tex->d.target == 2, nz = c->r.depth ? c->r.depth : 1;
    void *mb; uint64_t mo;
    if (!mtl_addr(c->src, img * nz, &mb, &mo)) { RED_ONCE(23, "缓冲→贴图拷贝的源不在内存池里"); return; }
    uint32_t upf; int ubpp;
    if (is_astc4(t.pixelFormat) && nvn_fmt_bc((uint32_t)c->tex->d.format)) {
        const uint8_t *src = [(__bridge id<MTLBuffer>)mb contents];
        if (!src) { RED_ONCE(46, "BC-to-ASTC copy source buffer is not CPU-readable"); return; }
        size_t ap = (size_t)((c->r.width + 3) / 4) * 16, aimg = ap * ((c->r.height + 3) / 4);
        id<MTLBuffer> tb = [g_dev newBufferWithLength:aimg * nz ?: 16 options:MTLResourceStorageModeShared];
        if (!tb) return;
        for (int z = 0; z < nz; z++) nvn_bc2astc((uint32_t)c->tex->d.format, src + mo + z * img, row, c->r.width, c->r.height, (uint8_t *)tb.contents + z * aimg, ap);
        res_add(tb);
        id<MTLBlitCommandEncoder> b = blit();
        for (int z = 0; z < nz; z++) enc_b_copy_buffer_texture(b, tb, z * aimg, ap, aimg, MTLSizeMake(c->r.width, c->r.height, 1), t, layer0 + c->r.zoffset + z, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, 0));
        defer_obj_release(tb);
        return;
    }
    if (bc_unpack(c->tex->d.format, &upf, &ubpp)) {   // No BC: the compressed blocks in the source are decompressed on the CPU into a temporary buffer, then copied pixel-wise into the decompressed-format texture
        const uint8_t *src = [(__bridge id<MTLBuffer>)mb contents];
        if (!src) { RED_ONCE(46, "无 BC 解压：缓冲→贴图拷贝的源缓冲 CPU 读不到"); return; }
        size_t up = (size_t)c->r.width * ubpp, uimg = up * c->r.height;
        id<MTLBuffer> tb = [g_dev newBufferWithLength:uimg * nz ?: 16 options:MTLResourceStorageModeShared];
        for (int z = 0; z < nz; z++) nvn_bc_decode(c->tex->d.format, src + mo + z * img, row, c->r.width, c->r.height, (uint8_t *)tb.contents + z * uimg, up);
        res_add(tb);
        id<MTLBlitCommandEncoder> b = blit();
        if (is3d) enc_b_copy_buffer_texture(b, tb, 0, up, uimg, MTLSizeMake(c->r.width, c->r.height, nz), t, 0, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, c->r.zoffset));
        else for (int z = 0; z < nz; z++) enc_b_copy_buffer_texture(b, tb, z * uimg, up, uimg, MTLSizeMake(c->r.width, c->r.height, 1), t, layer0 + c->r.zoffset + z, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, 0));
        defer_obj_release(tb);   // Release after this submit has finished running (and remove from residency)
        return;
    }
    id<MTLBlitCommandEncoder> b = blit();
    if (is3d)
        enc_b_copy_buffer_texture(b, (__bridge id<MTLBuffer>)mb, mo, row, img, MTLSizeMake(c->r.width, c->r.height, nz), t, 0, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, c->r.zoffset));
    else
        for (int z = 0; z < nz; z++)
            enc_b_copy_buffer_texture(b, (__bridge id<MTLBuffer>)mb, mo + z * img, row, img, MTLSizeMake(c->r.width, c->r.height, 1), t, layer0 + c->r.zoffset + z, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, 0));
}

// No BC + Legacy Device Pack: the game copies textures from files (already ASTC 4x4) whole into a BC3 texture array it creates itself (created as RGBA8 when there is no BC). On Switch the two formats have the same block size and
// the copy moves raw blocks; here it is changed to a compute kernel that decodes pixel by pixel and writes them in, with sRGB sources first read out as linear values and then encoded back to sRGB values, so the result = the values read in the destination format after moving the blocks as-is
static const char k_a2u_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"static float3 enc(float3 c) { return select(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, c <= 0.0031308); }\n"
"kernel void a2u(texture2d<float> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]], constant uint4 *p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
"  if (g.x >= p[1].x || g.y >= p[1].y) return; float4 v = src.read(g + p[0].xy); if (p[1].z) v.rgb = enc(v.rgb); dst.write(v, g + p[0].zw); }\n";
static int copy_t2t_astc(id<MTLTexture> s, id<MTLTexture> d, unsigned sl, unsigned ss, unsigned dl, unsigned ds, int sx, int sy, int dx, int dy, int w, int h) {
    static int failed; static id<MTLComputePipelineState> ps;
    if (failed) return 0;
    if (!ps) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_a2u_msl) options:nil error:&e]; id<MTLFunction> f = lib ? [lib newFunctionWithName:@"a2u"] : nil;
        ps = f ? nvn_cached_compute(g_dev, f, &e) : nil;
        if (!ps) { failed = 1; MLOG("【红】无 BC：ASTC → 解压格式拷贝的计算管线建不出 %s", e.localizedDescription.UTF8String); return 0; } }
    MTLTextureType tt = MTLTextureType2D;
    id<MTLTexture> sv = [s newTextureViewWithPixelFormat:s.pixelFormat textureType:tt levels:NSMakeRange(sl, 1) slices:NSMakeRange(ss, 1)];
    id<MTLTexture> dv = [d newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm textureType:tt levels:NSMakeRange(dl, 1) slices:NSMakeRange(ds, 1)];
    id<MTLBuffer> pb = [g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared];
    if (!sv || !dv || !pb) { RED_ONCE(47, "无 BC：ASTC → 解压格式拷贝建不出视图 / 参数块"); return 0; }
    uint32_t *p = pb.contents; p[0] = sx; p[1] = sy; p[2] = dx; p[3] = dy; p[4] = w; p[5] = h; p[6] = s.pixelFormat == MTLPixelFormatASTC_4x4_sRGB; p[7] = 0;
    res_add(pb);
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTLComputeCommandEncoder> ce = (id<MTLComputeCommandEncoder>)enc_c_begin(NULL);
    [ce setTexture:sv atIndex:0]; [ce setTexture:dv atIndex:1]; [ce setBuffer:pb offset:0 atIndex:0];
    enc_c_pso(ce, ps); enc_c_dispatch(ce, MTLSizeMake((w + 7) / 8, (h + 7) / 8, 1), MTLSizeMake(8, 8, 1)); enc_c_end(ce);
    args_dirty_all();
    defer_obj_unref(sv); defer_obj_unref(dv); defer_obj_release(pb);   // Release after this submit has finished running (views are not made resident separately; the parameter block is removed from residency)
    return 1;
}
// Texture copies a blit cannot do -- a format change, a scaled region or a mirrored one (MIRROR_X / MIRROR_Y) (Link's Awakening converts its 1408x792 RG11B10F scene to RGBA16F every frame; Super Mario 3D World scales 1920x1080 to 1280x720
// when leaving the title): one render pass per layer that draws the source region into the destination region, nearest or (LINEAR_FILTER) bilinear. Float, unorm and sRGB formats only, 2D / 2D array,
// destinations that can be render targets; anything else returns 0 and keeps the red line. Depth (Depth32Float / Depth32Float_Stencil8 into the same format) is drawn by writing depth from the fragment shader,
// always nearest; stencil is not carried over (Link's Awakening halves its 1408x792 depth-stencil buffer to 704x396)
static int fmt_int_or_ds(MTLPixelFormat f) {
    switch (f) {
        case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint: case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint: case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint:
        case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: case MTLPixelFormatRG16Uint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint:
        case MTLPixelFormatRGB10A2Uint: case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint:
        case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint: case MTLPixelFormatDepth16Unorm: case MTLPixelFormatDepth32Float: case MTLPixelFormatStencil8:
        case MTLPixelFormatDepth32Float_Stencil8: case MTLPixelFormatX32_Stencil8: return 1;
        default: return 0;
    }
}
static int copy_t2t_draw(id<MTLTexture> s, id<MTLTexture> d, unsigned sl, unsigned ss, unsigned dl, unsigned ds, const NVNcopyRegion *sr, const NVNcopyRegion *dr, int linear, int mirror) {
    static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *pso; static id<MTLFunction> vf, ff, fd; static id<MTLSamplerState> samp[2]; static id<MTLDepthStencilState> dss; static int failed;
    if (failed || sr->width <= 0 || sr->height <= 0 || dr->width <= 0 || dr->height <= 0) return 0;
    int depth = (s.pixelFormat == MTLPixelFormatDepth32Float || s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) && d.pixelFormat == s.pixelFormat, stencil = depth && d.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
    if (!depth && (fmt_int_or_ds(s.pixelFormat) || fmt_int_or_ds(d.pixelFormat))) return 0;
    if (mtl_fmt_block(d.pixelFormat) || !(d.usage & MTLTextureUsageRenderTarget) || s.sampleCount > 1 || d.sampleCount > 1) return 0;
    if (!pso) {
        static const char *src =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; };\n"
            "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
            // p[0] = source origin (texels) + source texels per destination texel, p[1] = destination origin + source level size
            "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4 *p [[buffer(0)]]) {\n"
            "  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; return t.sample(s, st / p[1].zw, level(0)); }\n"
            "struct D { float d [[depth(any)]]; };\n"
            "fragment D fsd(V in [[stage_in]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4 *p [[buffer(0)]]) {\n"
            "  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; D o; o.d = t.sample(s, st / p[1].zw, level(0)); return o; }\n";
        NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src) options:nil error:&e];
        vf = [lib newFunctionWithName:@"vs"]; ff = [lib newFunctionWithName:@"fs"]; fd = [lib newFunctionWithName:@"fsd"];
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new]; dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = YES; dss = [g_dev newDepthStencilStateWithDescriptor:dd];
        for (int i = 0; i < 2; i++) { MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = i ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge; sd.supportArgumentBuffers = YES; samp[i] = [g_dev newSamplerStateWithDescriptor:sd]; }
        if (!vf || !ff || !fd || !dss || !samp[0] || !samp[1]) { failed = 1; MLOG("【红】换格式 / 缩放贴图拷贝的管线建不出：%s", e.localizedDescription.UTF8String); return 0; }
        pso = [NSMutableDictionary dictionary];
    }
    id<MTLRenderPipelineState> ps = pso[@(d.pixelFormat)];
    if (!ps) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = vf; pd.fragmentFunction = depth ? fd : ff;
        if (depth) { pd.depthAttachmentPixelFormat = d.pixelFormat; if (stencil) pd.stencilAttachmentPixelFormat = d.pixelFormat; } else pd.colorAttachments[0].pixelFormat = d.pixelFormat;
        NSError *e = nil; ps = nvn_cached_render(g_dev, pd, &e);
        if (!ps) { RED_ONCE(83, "换格式 / 缩放贴图拷贝：目标格式 %lu 的管线建不出：%s", (unsigned long)d.pixelFormat, e.localizedDescription.UTF8String); return 0; }
        pso[@(d.pixelFormat)] = ps;
    }
    unsigned sw = MAX(s.width >> sl, 1ul), sh = MAX(s.height >> sl, 1ul), dw = MAX(d.width >> dl, 1ul), dh = MAX(d.height >> dl, 1ul);
    if ((unsigned)(dr->xoffset + dr->width) > dw || (unsigned)(dr->yoffset + dr->height) > dh) return 0;
    int nz = sr->depth ? sr->depth : 1;
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    for (int z = 0; z < nz; z++) {
        id<MTLTexture> sv = [s newTextureViewWithPixelFormat:s.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(sl, 1) slices:NSMakeRange(ss + sr->zoffset + z, 1)];
        id<MTLBuffer> pb; size_t po = arena_alloc(32, &pb);   // parameters in the frame arena (resident already): a fresh 32-byte buffer per copy was a residency add, a commit and a release (Link's Awakening, about 11 a frame)
        if (!sv || !pb) { RED_ONCE(84, "换格式 / 缩放贴图拷贝：建不出源视图 / 参数块"); return z > 0; }
        float *p = (float *)((uint8_t *)pb.contents + po); p[0] = sr->xoffset; p[1] = sr->yoffset; p[2] = (float)sr->width / dr->width; p[3] = (float)sr->height / dr->height;
        if (mirror & NVN_COPY_FLAGS_MIRROR_X_BIT) { p[0] += sr->width; p[2] = -p[2]; }   // mirrored: walk the source region from its far edge
        if (mirror & NVN_COPY_FLAGS_MIRROR_Y_BIT) { p[1] += sr->height; p[3] = -p[3]; }
        p[4] = dr->xoffset; p[5] = dr->yoffset; p[6] = sw; p[7] = sh;
        int whole = !dr->xoffset && !dr->yoffset && (unsigned)dr->width == dw && (unsigned)dr->height == dh;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
        MTLRenderPassAttachmentDescriptor *a = depth ? (MTLRenderPassAttachmentDescriptor *)rp.depthAttachment : (MTLRenderPassAttachmentDescriptor *)rp.colorAttachments[0];
        a.texture = d; a.level = dl; a.slice = ds + dr->zoffset + z; a.loadAction = whole ? MTLLoadActionDontCare : MTLLoadActionLoad; a.storeAction = MTLStoreActionStore;
        if (stencil) { MTLRenderPassStencilAttachmentDescriptor *st = rp.stencilAttachment; st.texture = d; st.level = dl; st.slice = a.slice; st.loadAction = MTLLoadActionLoad; st.storeAction = MTLStoreActionStore; }
        ser_gate(); id<MTLRenderCommandEncoder> en = enc_r_begin(rp);
        if (depth) enc_r_dss(en, dss);

        [en setFragmentTexture:sv atIndex:0]; [en setFragmentSamplerState:samp[!!linear && !depth] atIndex:0]; [en setFragmentBuffer:pb offset:po atIndex:0];
        [en setViewport:(MTLViewport){dr->xoffset, dr->yoffset, dr->width, dr->height, 0, 1}];
        [en setScissorRect:(MTLScissorRect){(NSUInteger)dr->xoffset, (NSUInteger)dr->yoffset, (NSUInteger)dr->width, (NSUInteger)dr->height}];
        enc_r_pso(en, ps); [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; enc_r_end(en);
        defer_obj_unref(sv);   // released after this submission has run
    }
    args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
    { static int once[2]; if (!once[depth]++) MLOG("换格式 / 缩放贴图拷贝：首例 %dx%d 格式 %lu → %dx%d 格式 %lu%s%s", sr->width, sr->height, (unsigned long)s.pixelFormat, dr->width, dr->height, (unsigned long)d.pixelFormat,
                                                    linear && !depth ? "（双线性）" : "", stencil ? "（只拷深度，模板不拷）" : ""); }
    return 1;
}
// GPU: texture -> texture (previously mislisted as a no-op, 3 times per frame on the character select page). Same size, not mirrored, same format -> blit layer by layer; the other semantics (scaling with filtering, mirroring, format change) cannot be expressed by blit: report a failure, to be implemented
static void copy_t2t(const p_copy_t2t *c) {
    za_diag_op("copy-texture-source", c->src); za_diag_op("copy-texture-destination", c->dst);
    if (c->src) xv_check(c->src->serial, c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, "贴图拷贝源");
    rp_flush();   // Land the pending pure-clear render pass first (zero-source candidates only enter the set when the render pass ends); the blit() below would end the render pass anyway
    if (c->dst) zskip_del(c->dst->serial);
    if (c->src && c->dst && c->sr.xoffset == 0 && c->sr.yoffset == 0 && c->dr.xoffset == 0 && c->dr.yoffset == 0
        && c->sr.width == (int)c->src->d.width && c->sr.height == (int)c->src->d.height && c->dr.width == (int)c->dst->d.width && c->dr.height == (int)c->dst->d.height) {
        int sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, dl = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0;
        int ss = c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, ds = c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0;
        if (zskip_has(c->src->serial, sl, ss)) zskip_add(c->dst->serial, dl, ds);
    }
    id<MTLTexture> s = tex_mtl(c->src), d = tex_mtl(c->dst);
    PD("T src=%llu dst=%llu %dx%dx%d→%dx%dx%d flags=%#x", c->src->serial, c->dst->serial, c->sr.width, c->sr.height, c->sr.depth, c->dr.width, c->dr.height, c->dr.depth, c->flags);
    if (!s || !d) return;
    unsigned sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, dl = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0;
    unsigned sz0 = c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, dz0 = c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0;
    if (c->flags & DM_COPY_RESOLVE) {   // Downsample: multisample resolve into the single-sample destination (depth takes sample 0); with equal sample counts it is a plain whole copy
        if (s.sampleCount == d.sampleCount && s.pixelFormat == d.pixelFormat && s.width == d.width && s.height == d.height) {
            enc_b_copy_texture(blit(), s, sz0, sl, MTLOriginMake(0, 0, 0), MTLSizeMake(s.width, s.height, 1), d, dz0, dl, MTLOriginMake(0, 0, 0));
            return;
        }
        if (s.sampleCount <= 1 || d.sampleCount != 1 || s.pixelFormat != d.pixelFormat || s.width != d.width || s.height != d.height) {
            RED_ONCE(72, "Downsample 未实现的形态：%lux%lu %lu 样本 格式 %lu → %lux%lu %lu 样本 格式 %lu", (unsigned long)s.width, (unsigned long)s.height, (unsigned long)s.sampleCount,
                     (unsigned long)s.pixelFormat, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.sampleCount, (unsigned long)d.pixelFormat);
            return;
        }
        rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
        MTLPixelFormat pf = s.pixelFormat;
        int depth = pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth16Unorm || pf == MTLPixelFormatDepth32Float_Stencil8;
        MTLRenderPassAttachmentDescriptor *a = depth ? rp.depthAttachment : rp.colorAttachments[0];
        a.texture = s; a.level = sl; a.slice = sz0; a.resolveTexture = d; a.resolveLevel = dl; a.resolveSlice = dz0;
        a.loadAction = MTLLoadActionLoad; a.storeAction = MTLStoreActionStoreAndMultisampleResolve;
        if (depth) rp.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
        ser_gate(); enc_r_end(enc_r_begin(rp));
        return;
    }
    PD("t src=%llu lv%u sl%u xyz=%d,%d,%d dst=%llu lv%u sl%u xyz=%d,%d,%d size=%d,%d,%d", c->src->serial, sl, sz0, c->sr.xoffset, c->sr.yoffset, c->sr.zoffset,
       c->dst->serial, dl, dz0, c->dr.xoffset, c->dr.yoffset, c->dr.zoffset, c->sr.width, c->sr.height, c->sr.depth);
    int s3 = c->src->d.target == 2, d3 = c->dst->d.target == 2, nz = c->sr.depth ? c->sr.depth : 1;
    if (no_bc() && (s.pixelFormat == MTLPixelFormatASTC_4x4_LDR || s.pixelFormat == MTLPixelFormatASTC_4x4_sRGB) && (d.pixelFormat == MTLPixelFormatRGBA8Unorm || d.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB)
        && !s3 && !d3 && c->sr.width == c->dr.width && c->sr.height == c->dr.height && (d.usage & MTLTextureUsageShaderWrite)) {   // No BC: ASTC source -> decompressed-format destination (see copy_t2t_astc)
        int ok = 1;
        for (int z = 0; z < nz && ok; z++) ok = copy_t2t_astc(s, d, sl, sz0 + c->sr.zoffset + z, dl, dz0 + c->dr.zoffset + z, c->sr.xoffset, c->sr.yoffset, c->dr.xoffset, c->dr.yoffset, c->sr.width, c->sr.height);
        if (ok) return;
    }
    if ((c->sr.width != c->dr.width || c->sr.height != c->dr.height || s.pixelFormat != d.pixelFormat || (c->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT))) && !s3 && !d3
        && (c->sr.depth ? c->sr.depth : 1) == (c->dr.depth ? c->dr.depth : 1)
        && !(c->flags & ~(NVN_COPY_FLAGS_ENGINE_2D_BIT | NVN_COPY_FLAGS_LINEAR_FILTER_BIT | NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT))
        && copy_t2t_draw(s, d, sl, sz0, dl, dz0, &c->sr, &c->dr, !!(c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT), c->flags)) return;   // format change / scaling / mirroring: draw it (see copy_t2t_draw)
    if (c->sr.width != c->dr.width || c->sr.height != c->dr.height || (c->sr.depth ? c->sr.depth : 1) != (c->dr.depth ? c->dr.depth : 1) || (c->flags & ~NVN_COPY_FLAGS_ENGINE_2D_BIT & ~NVN_COPY_FLAGS_LINEAR_FILTER_BIT)
        || s.pixelFormat != d.pixelFormat || s3 != d3) {
        RED_ONCE(33, "贴图→贴图拷贝未实现的形态：%dx%dx%d→%dx%dx%d 标志 %#x 格式 %lu→%lu 3D %d→%d", c->sr.width, c->sr.height, c->sr.depth, c->dr.width, c->dr.height, c->dr.depth,
                 c->flags, (unsigned long)s.pixelFormat, (unsigned long)d.pixelFormat, s3, d3);
        return;
    }
    id<MTLBlitCommandEncoder> b = blit();
    if (s3)
        enc_b_copy_texture(b, s, 0, sl, MTLOriginMake(c->sr.xoffset, c->sr.yoffset, c->sr.zoffset), MTLSizeMake(c->sr.width, c->sr.height, nz), d, 0, dl, MTLOriginMake(c->dr.xoffset, c->dr.yoffset, c->dr.zoffset));
    else
        for (int z = 0; z < nz; z++)
            enc_b_copy_texture(b, s, sz0 + c->sr.zoffset + z, sl, MTLOriginMake(c->sr.xoffset, c->sr.yoffset, 0), MTLSizeMake(c->sr.width, c->sr.height, 1), d, dz0 + c->dr.zoffset + z, dl, MTLOriginMake(c->dr.xoffset, c->dr.yoffset, 0));
    // Diagnostic DM_NVN_T2T_TRACE=WxH:file: after each copy into an RGBA16F texture of that size, copy the result once more into a trace buffer on the GPU timeline (frame numbers kept on the CPU side); after 240 copies write the file
    { static int on = -1; static uint32_t tw, th; static const char *tf; static id<MTLBuffer> tb; static int tn; static uint64_t tfr[240], from;
      if (on < 0) { const char *e = getenv("DM_NVN_T2T_TRACE"), *fe = getenv("DM_NVN_T2T_FROM"); on = e && sscanf(e, "%ux%u", &tw, &th) == 2 && (tf = strchr(e, ':')) && *++tf; from = fe ? strtoull(fe, NULL, 10) : 0; }
      if (on > 0 && tn < 240 && atomic_load(&g_cur_frame) >= from && d.width == tw && d.height == th && d.pixelFormat == MTLPixelFormatRGBA16Float && !s3) {
          if (!tb) { tb = [g_dev newBufferWithLength:(size_t)240 * tw * th * 8 options:MTLResourceStorageModeShared]; res_add(tb); }
          enc_b_copy_texture_buffer(blit(), d, dz0, dl, MTLOriginMake(0, 0, 0), MTLSizeMake(tw, th, 1), tb, (size_t)tn * tw * th * 8, (size_t)tw * 8, (size_t)tw * th * 8);
          tfr[tn++] = atomic_load(&g_cur_frame);
          if (tn == 240) {
              rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
              mtl_finish_gpu();
              FILE *f = fopen(tf, "wb");
              if (f) { fprintf(f, "%u %u %d\n", tw, th, tn); fwrite(tfr, 8, 240, f); fwrite(tb.contents, 1, (size_t)240 * tw * th * 8, f); fclose(f); }
              MLOG("[t2t-trace] %d 次 → %s（帧 %llu–%llu）", tn, tf, (unsigned long long)tfr[0], (unsigned long long)tfr[239]);
          }
      }
    }
}

// GPU: clear a texture region with the given value (previously mislisted as a no-op, called 5 times at initialization; the initial value of a Metal private texture is undefined, and the game relies on the cleared initial value).
// Whole level and whole face, all four channels written -> open one clear-only render pass per layer / depth slice (the hardware converts the clear value by the texture format, including integer and depth);
// view with changed format: clear on a view of that format; partial region, partial channels, compressed / packaged textures (no render target usage): report a failure, to be implemented
static void clear_tex(const p_clear_tex *c) {
    za_diag_op("clear-texture", c->tex);
    if (c->tex) zskip_del(c->tex->serial);
    id<MTLTexture> t = tex_mtl(c->tex);
    PD("X ser=%llu 视图格式 %#x 区域 %d,%d,%d %dx%dx%d 掩码 %#x %s %g,%g,%g,%g", c->tex->serial, c->has_view && c->view.set_format ? c->view.format : -1, c->r.xoffset, c->r.yoffset, c->r.zoffset, c->r.width, c->r.height, c->r.depth, c->mask,
       c->is_uint ? "ui" : "f", c->is_uint ? (double)c->c.u[0] : c->c.f[0], c->is_uint ? (double)c->c.u[1] : c->c.f[1], c->is_uint ? (double)c->c.u[2] : c->c.f[2], c->is_uint ? (double)c->c.u[3] : c->c.f[3]);
    if (!t) return;
    unsigned lv = c->has_view && c->view.set_levels ? c->view.base_level : 0, l0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    NSUInteger w = MAX(t.width >> lv, 1u), h = MAX(t.height >> lv, 1u);
    int is3d = t.textureType == MTLTextureType3D, nz = c->r.depth ? c->r.depth : 1;
    if (c->has_view && c->view.set_format) {   // View with changed format: the clear value is interpreted in the view format (all 5 of the game's calls are the ui version with an integer view), and the clear is done on a view of the same format
        uint32_t vpf = 0; uint8_t swz[4];
        if (!nvn_fmt_to_mtl(c->view.format, &vpf, swz) || !vpf) { RED_ONCE(42, "清贴图视图格式 %#x 无对应", c->view.format); return; }
        if (tex_need_pfv(t.pixelFormat, (MTLPixelFormat)vpf) && !(t.usage & MTLTextureUsagePixelFormatView)) t = tex_upgrade_pfv(c->tex, t, (MTLPixelFormat)vpf);
        if (tex_need_pfv(t.pixelFormat, (MTLPixelFormat)vpf)) {
            id<MTLTexture> v = [t newTextureViewWithPixelFormat:(MTLPixelFormat)vpf];
            if (!v) { RED_ONCE(43, "清贴图建不出视图：格式 %lu → %u", (unsigned long)t.pixelFormat, vpf); return; }
            t = v;
        }
    }
    MTLPixelFormat pf = t.pixelFormat;
    int depth = pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth16Unorm || pf == MTLPixelFormatDepth32Float_Stencil8, stencil = pf == MTLPixelFormatStencil8 || pf == MTLPixelFormatDepth32Float_Stencil8;
    if (c->r.xoffset || c->r.yoffset || (NSUInteger)c->r.width != w || (NSUInteger)c->r.height != h || (c->mask & 0xf) != 0xf
        || !(t.usage & MTLTextureUsageRenderTarget)) {
        RED_ONCE(44, "清贴图未实现的形态：区域 %d,%d %dx%d（本级 %lux%lu）掩码 %#x 可作渲染目标 %d", c->r.xoffset, c->r.yoffset, c->r.width, c->r.height,
                 (unsigned long)w, (unsigned long)h, c->mask, !!(t.usage & MTLTextureUsageRenderTarget));
        return;
    }
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    for (int z = 0; z < nz; z++) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
        MTLRenderPassAttachmentDescriptor *a = depth ? rp.depthAttachment : stencil ? rp.stencilAttachment : rp.colorAttachments[0];
        a.texture = t; a.level = lv; a.loadAction = MTLLoadActionClear; a.storeAction = MTLStoreActionStore;
        if (is3d) a.depthPlane = c->r.zoffset + z; else a.slice = l0 + c->r.zoffset + z;
        if (depth) { rp.depthAttachment.clearDepth = c->c.f[0];
            if (stencil) { rp.stencilAttachment.texture = t; rp.stencilAttachment.level = lv; rp.stencilAttachment.slice = a.slice; rp.stencilAttachment.loadAction = MTLLoadActionClear;
                           rp.stencilAttachment.storeAction = MTLStoreActionStore; rp.stencilAttachment.clearStencil = c->c.u[1]; } }
        else if (stencil) rp.stencilAttachment.clearStencil = c->c.u[0];
        else rp.colorAttachments[0].clearColor = c->is_uint ? MTLClearColorMake(c->c.u[0], c->c.u[1], c->c.u[2], c->c.u[3]) : MTLClearColorMake(c->c.f[0], c->c.f[1], c->c.f[2], c->c.f[3]);
        ser_gate(); enc_r_end(enc_r_begin(rp));
    }
}

// Context description of one submit (mtl_sub_commit takes one per submit and prints it into the failure line when the command buffer errors): for the GPU memory fault on the first frame entering Travincal, the failure line had only the sequence number and error code, and it was impossible to tell what was in that submit
static const char *cctx_snap(void) {
    static char b[320]; static uint64_t p_pass, p_up, p_reup, p_ser; uint64_t up = 0, reup = 0; mtl_pkg_stats(&up, &reup);
    pthread_mutex_lock(&g_deferred_mu); unsigned long nd = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    snprintf(b, sizeof b, "帧 %llu；自上次提交：通道 +%llu、贴图首传 +%llu、重传 +%llu、序号表变动 +%llu；延迟释放队列 %lu 个；世界冻结状态 %d",
             (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)(g_stat_pass - p_pass), (unsigned long long)(up - p_up), (unsigned long long)(reup - p_reup),
             (unsigned long long)(g_ser_gen - p_ser), nd, g_frz_state);
    p_pass = g_stat_pass; p_up = up; p_reup = reup; p_ser = g_ser_gen; return b;
}
// ---------- Do not draw the world while the world is frozen (the whole game can be frozen while a panel is open) ----------
// The "world frozen" status bit g_frz_on is set by mtl_world_frozen() (the host's freeze handshake calls it; in tests DM_NVN_TEST_FRZ=1 can stand in, or it can be switched in-process via @set:g_frz_on of DM_NVN_UB).
// This code does not judge by itself whether a panel is open. Structure of a frame (3.3.94012 in-game, per-pass dump): UI pass -> [UI stencil pass] -> about 40 passes of shadows / depth / lighting / transparency / post-processing -> final compositing pass (writes the window texture).
// The first frame after the bit is set = full frame: draw everything as usual, dropping only the draws of the UI stencil pass (stencil all 0 = no pixels blocked by the panel) -- post-processing normally does not write pixels blocked by the panel, so a shrinking panel would expose old content;
// from the second frame on = frozen frame: from the first render pass without a color attachment up to before the render pass that writes the window texture, set-target / clear / draw / texture-to-texture copy / compute are all not executed, the scene result stays as the full frame's copy, and the UI pass and the final compositing pass are drawn as usual.
// Frame boundaries are identified by render pass: the first set-target without the window texture after the window texture has been written = start of a new frame, and the status bit is latched here. Unrecognized structure (no render pass without color in a frame) = skip nothing.
// State constants / buffer updates / events / counters / texture uploads execute as usual (the UI pass needs them, the game is waiting).
int g_frz_on = -1;
static int g_frz_phase = 2, g_frz_drop, g_frz_seen_ds; static uint64_t g_frz_full_n, g_frz_skip_n;
void mtl_world_frozen(int on) { g_frz_on = on != 0; }
static int frz_wanted(void) { if (g_frz_on < 0) { const char *e = getenv("DM_NVN_TEST_FRZ"); g_frz_on = e && atoi(e); if (g_frz_on) MLOG("DM_NVN_TEST_FRZ：世界冻结状态位由测试开关顶替（常开）"); } tbdr_on(); return g_frz_on > 0 && tbdr_on() && g_ruler_truth <= 0; }
// ---------- Runtime extrapolation (power-saving rendering method; no switch for the player; it caused a stutter defect in released builds, now off by default, to be re-enabled once there is evidence of no stutter on A17) ----------
// 3D every other frame: a fully drawn frame draws as usual and, before the attachments are repurposed, copies the opaque HDR (468), the main depth-stencil, the post-tone-mapping 468 and the post-FXAA 469;
// extrapolated frame: the UI stencil pass runs as usual; shadow map / depth prepass / main pass keep only the draws of moving units (skinned); before the first skinned draw of the depth prepass run one extrapolation pass (nvn_rtex.inc: by cb3 +464 -> +336
// move the previous frame's HDR and depth to this frame, treat old unit positions 0x10 / UI cutout 0x2 as holes to fill, and export stencil 0x80 for holes and pixels newly exposed at the screen edge); translucents are drawn as usual and write effect bit 0x40;
// compositing / add bloom / add fog / tone mapping / FXAA really run only in the 0xD0 region, and outside the region the previous frame's LDR result is copied in with a whole-image shift (orthographic camera, rounded); full-screen items such as the bloom chain are skipped; sharpening upscale and UI compositing run as usual.
// Fallback to the original rendering method: version file render_safe = true (dm_update.m), the extrapolation pipeline cannot be built (whole run), test DM_RT_AB=0. When a panel is open and the world is frozen, yield to the freeze path.
// On a camera jump (orientation change or translation over 64 pixels) and for 4 frames before and after switching the 3D render scale, whole frames are fully drawn. Unit shadows on the ground keep using the previous fully drawn frame.
static int rt_is_skin(void) {   // Moving units = the 10 whose vertex shader reads constants by bone index (the key table is taken from the Mac shader cache b-424a072084e6, identified by the dynamic vp_c5 index in the source)
    __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil; if (!pr || !pr->sh[0]) return 0;
    static const char *k[] = { "0fa4eb7559f1637a", "1d97d33ab66abcba", "3deaf68fd65b5298", "4dffa355f2fbcdaf", "54c93fa22abff9ee",
                               "64dd7d022f3fae39", "67e2ebc28c830f44", "add86ee8e20f89b9", "b3ae9be5cc670f35", "fbf6dc1873a449e9" };
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) if (!strncmp(pr->sh[0]->key, k[i], 16)) return 1;
    return 0;
}
int g_dm_rt_ab = 0;   // Default is the original rendering method (the new method stutters in-game on A17 iPhone 15 Pro and ghosts characters on the character select page, so off for now); DM_RT_AB=1 turns on the new method for testing. The version file render_safe can only set it to 0, no effect
static struct { int env, frame, filt, first, hdrp, mainp_seen, skipall, oit_seen, tm_seen, fx_state, comp_seen; uint64_t ldr_ser; float vpc[16], vpp[16]; id<MTLTexture> ldr, pl468, pl469; int ui_seen, main_seen, copied, ex_done, shadow, have_prev, have_ldr, sdx, sdy, cam_checked, force_full, disabled, was_on; uint64_t n_jump; uint64_t ds_ser, hdr_ser, n_ex, n_full, n_noex;
    id<MTLTexture> ds, hdr, pds, phdr; struct { uint64_t key; int skin, other; } sh[16]; int nsh, cur_sh; } g_rt = { .env = -1 };
static int rt_want(void);
static void rt_toggle_poll(void) { static int tg = -1; if (tg < 0) { const char *e = getenv("DM_RT_AB_TOGGLE"); tg = e ? atoi(e) : 0; (void)rt_want(); }   // On first call also read DM_RT_AB (on a real device the world-freeze bit is set to 0 very early, so frz_op is not necessarily called)   // test: switch back and forth between A / B every tg seconds by wall clock
    if (tg > 0) { int v = (int)((time(NULL) / tg) & 1); if (v != g_dm_rt_ab) { g_dm_rt_ab = v; MLOG("DM_RT_AB_TOGGLE：画法切到 %s", v ? "B" : "A"); } } }
void mtl_rt_force_full(int n) { g_rt.force_full = n; }   // Called by the host: before an overall picture change such as switching the 3D render scale, the following n frames are all fully drawn
// ---------- Power-saving mode (30 frames) ----------
// On: the display link object changes to 30 Hz (frame rate range 30–30, a ProMotion screen lowers its refresh rate accordingly; the frame rate hint in front_ios.m is lowered together), the game presents one frame per display-link tick; runtime extrapolation off (really draws every frame).
// Game logic already runs at 25 ticks / second and unit positions are interpolated by time, so there are no repeated frames at 30 frames (measured on a real device: coefficient of variation of per-frame camera displacement 0.292 at 30 frames, 0.296 at 60 frames).
// The switch is on the 「自定义」(Custom) page (dm_custom.c eco_fps, off by default, takes effect on apply); the test switch DM_FPS=30/60 pins it.
__attribute__((weak)) int dm_fps_setting(void) { return 60; }   // The host's dm_custom.c provides the strong definition; always 60 when graphics unit tests do not link the host
// When the game itself asks for 30 fps (pacing by the game's present interval, interval 2, hle_nvn.c) the display link runs at 30 Hz too: games whose logic counts frames, such as Engage, are not pulled up to 60 fps by the display link on iOS
__attribute__((weak)) int nvn_game_fps(void) { return 60; }   // hle_nvn.c provides the strong definition
int mtl_fps_target(void) { static int env = -2; if (env == -2) { const char *e = getenv("DM_FPS"); env = e ? atoi(e) : -1; if (env != 30 && env != 60) env = -1; else MLOG("DM_FPS=%d：帧数由测试开关钉住", env); }
    return env > 0 ? env : dm_fps_setting() == 30 || nvn_game_fps() == 30 ? 30 : 60; }
// Whenever the extrapolation enabled state changes (power-saving mode toggled, world freeze takes over / hands back): first let the "no store across frames" ledger relearn (during the trial period it stores as usual), and the extrapolation really switches one frame later --
// guaranteeing the last submit before the switch is a full store. Otherwise attachments cleared first every frame at 30 frames are learned as no-store, and the first extrapolated frame after switching back to 60 frames reads old content and flashes for one frame (measured on a test build; only reverted after the xv self-check reported a failure)
static void xv_relearn(const char *why) { g_xv_n = 0; g_xv_ntrial = 0; g_xv_ntouch = 0; MLOG("跨帧不写回：%s，账本清空重学（试用期内照常写回）", why); }
static int rt_want_raw(void);
static int rt_want(void) {
    static int eff = -1; static uint64_t pend; int want = rt_want_raw(); uint64_t f = atomic_load(&g_cur_frame);
    if (eff < 0) eff = want;
    if (want == eff) { pend = 0; return eff; }
    if (!pend) { pend = f + 1; xv_relearn(want ? "外推将启用" : "外推将停用"); return eff; }
    if (f > pend) { eff = want; pend = 0; }
    return eff;
}
static int rt_want_raw(void) { static int forced; if (g_rt.env < 0) { const char *e = getenv("DM_RT_AB"); g_rt.env = e ? atoi(e) : 0; forced = e != NULL; if (e) MLOG("DM_RT_AB=%d：画法由环境变量钉住（测试用，自定义页的开关不再起作用）", g_rt.env); }
    if (forced) g_dm_rt_ab = g_rt.env;
    return g_dm_rt_ab == 1 && !g_rt.disabled && g_frz_on <= 0 && tbdr_on() && g_ruler_truth <= 0 && mtl_fps_target() == 60; }   // When a panel is open and the world is frozen, yield to the freeze path (saves more); power-saving mode (30 frames) really draws every frame
#include "nvn_rtex.inc"
static uint64_t rt_key(const p_set_rt *r) { uint64_t s_; int lv, sl; rt_sub(r, 8, &s_, &lv, &sl); return s_ * 64 + (uint64_t)lv * 8 + (uint64_t)sl; }
static int rt_is_shadow(const p_set_rt *r) { return !r->n && r->depth && r->depth->d.width == r->depth->d.height; }
static id<MTLTexture> rt_cp(id<MTLTexture> src, id<MTLTexture> dst0) {
    id<MTLTexture> dst = dst0;
    if (!dst || dst.width != src.width || dst.height != src.height || dst.pixelFormat != src.pixelFormat) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat width:src.width height:src.height mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView; d.storageMode = MTLStorageModePrivate;
        if (dst0) defer_obj_release(dst0);   // Size change: the old copy may still be read by an in-flight command buffer (MTL4 command buffers do not retain resources), so release it deferred
        dst = [g_dev newTextureWithDescriptor:d]; if (!dst) return nil; res_add(dst); }
    id<MTLBlitCommandEncoder> b = blit(); enc_b_copy_texture(b, src, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(src.width, src.height, 1), dst, 0, 0, MTLOriginMake(0, 0, 0)); return dst; }
static void rt_blit_shift(id<MTLTexture> src, id<MTLTexture> dst, int dx, int dy) {   // Integer-offset copy: dst(x + dx, y + dy) = src(x, y); the strip at the screen edge that cannot be moved is left for the region recompute (the extrapolation pass has already marked it 0x80)
    if (!src || !dst || src.width != dst.width || src.height != dst.height) return;
    int W = (int)src.width, H = (int)src.height, w = W - abs(dx), h = H - abs(dy); if (w <= 0 || h <= 0) return;
    id<MTLBlitCommandEncoder> b = blit(); enc_b_copy_texture(b, src, 0, 0, MTLOriginMake(MAX(0, -dx), MAX(0, -dy), 0), MTLSizeMake(w, h, 1), dst, 0, 0, MTLOriginMake(MAX(0, dx), MAX(0, dy), 0)); }
static void rt_copy_prev(void) {   // Fully drawn frame: copy the opaque HDR and the main depth-stencil once each (in order within the GPU, in this command buffer)
    if (g_rt.copied || !g_rt.main_seen || !g_rt.hdr || !g_rt.ds) return; g_rt.copied = 1;   // Do not copy when the main pass was not recognized this frame (foreground / loading screen): the textures recognized last time may already have been released by the game and not be in the residency set
    if (!rx_setup(g_rt.hdr.pixelFormat, g_rt.ds.pixelFormat)) { if (!g_rt.disabled) { g_rt.disabled = 1; MLOG("【红】运行时外推：外推管线在这台设备上建不出，整局退回原画法"); } return; }   // Cannot be built (old GPU does not support stencil export etc.): fall back for the whole run
    g_rt.phdr = rt_cp(g_rt.hdr, g_rt.phdr); g_rt.pds = rt_cp(g_rt.ds, g_rt.pds); g_rt.have_prev = g_rt.phdr && g_rt.pds; }
static void rt_frame_begin(void) { g_rt.filt = g_rt.ui_seen = g_rt.main_seen = g_rt.copied = g_rt.ex_done = g_rt.mainp_seen = 0; g_rt.cur_sh = -1;
    g_rt.oit_seen = g_rt.tm_seen = g_rt.fx_state = g_rt.comp_seen = g_rt.skipall = g_rt.cam_checked = 0; g_rt_ovr = 0; g_dss_dirty = 1;
    if (g_frz_state == 0) { g_rt.have_prev = g_rt.have_ldr = 0; for (int i = 0; i < g_rt.nsh; i++) g_rt.sh[i].skin = g_rt.sh[i].other = 0; } }   // Fully drawn frame: the next frame may extrapolate only if this frame's copies succeeded
static void rt_set_rt(const p_set_rt *r, int win) {   // Every set-target (both fully drawn and extrapolated frames go through here): identify roles, find the copy points in fully drawn frames, and in extrapolated frames decide whether this render pass keeps only skinned draws
    g_rt.cur_sh = -1; g_rt.shadow = 0; g_rt.first = 1; g_rt.hdrp = 0;
    if (win) { if (g_frz_state == 0) { rt_copy_prev(); if (g_rt.fx_state == 2 && g_rt.ldr) { g_rt.pl469 = rt_cp(g_rt.ldr, g_rt.pl469); g_rt.have_ldr = g_rt.pl469 != nil; g_rt.fx_state = 3; } }
        g_rt.filt = 0; g_rt.skipall = 0; if (g_rt_ovr) { g_rt_ovr = 0; g_dss_dirty = 1; } return; }
    id<MTLTexture> c0 = r->n > 0 && r->color[0] ? tex_mtl(r->color[0]) : nil, dt = r->depth ? tex_mtl(r->depth) : nil;
    int hdr_here = 0; for (int i = 0; i < r->n && i < 8; i++) if (r->color[i] && g_rt.hdr_ser && r->color[i]->serial == g_rt.hdr_ser) hdr_here = 1;
    if (g_frz_state == 0) {
        if (!g_rt.main_seen && r->n == 1 && c0 && c0.pixelFormat == MTLPixelFormatRG11B10Float && dt && dt.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 && dt.width >= 500 && dt.width != dt.height) {
            { static uint64_t lh, ld; if (lh != r->color[0]->serial || ld != r->depth->serial) { lh = r->color[0]->serial; ld = r->depth->serial; MLOG("运行时外推：认出主通道 不透明 HDR 序号 %llu %lux%lu、主深度模板序号 %llu %lux%lu（通道 %llu）", (unsigned long long)lh, (unsigned long)c0.width, (unsigned long)c0.height, (unsigned long long)ld, (unsigned long)dt.width, (unsigned long)dt.height, g_stat_pass + 1); } }
            g_rt.main_seen = 1; g_rt.hdr_ser = r->color[0]->serial; g_rt.ds_ser = r->depth->serial; g_rt.hdr = c0; g_rt.ds = dt; }
        else if (g_rt.main_seen && hdr_here && !g_rt.copied) rt_copy_prev();   // The first set-target after the main pass that attaches the opaque HDR again (the translucent pass; 468 occupies slot 0 but is not written): before it, copy the HDR and the main depth-stencil
        else if (g_rt.main_seen && hdr_here && r->n == 1 && r->depth && r->depth->serial == g_rt.ds_ser) g_rt.fx_state = 1;   // Only 468 + main depth attached = tone mapping (repurposes 468 as the LDR output)
        else if (g_rt.fx_state == 1) {   // The first render pass after tone mapping = FXAA (reads 468, writes 469): before it, copy the post-tone-mapping 468 (LDR) and note down 469
            if (r->n >= 1 && r->color[0] && c0) { g_rt.ldr_ser = r->color[0]->serial; g_rt.ldr = c0; g_rt.pl468 = rt_cp(g_rt.hdr, g_rt.pl468); g_rt.fx_state = 2; } else g_rt.fx_state = 0; }
        else if (g_rt.fx_state == 2 && g_rt.ldr) { g_rt.pl469 = rt_cp(g_rt.ldr, g_rt.pl469); g_rt.have_ldr = g_rt.pl468 && g_rt.pl469; g_rt.fx_state = 3; }   // Before the first render pass after FXAA: copy the post-FXAA 469
    }
    if (rt_is_shadow(r)) { uint64_t k = rt_key(r); int i; for (i = 0; i < g_rt.nsh && g_rt.sh[i].key != k; i++) {}
        if (i == g_rt.nsh && i < 16) { g_rt.sh[i].key = k; g_rt.sh[i].skin = g_rt.sh[i].other = 0; g_rt.nsh++; } g_rt.cur_sh = i < 16 ? i : -1; g_rt.shadow = 1; }
    int ds_here = r->depth && g_rt.ds_ser && r->depth->serial == g_rt.ds_ser;
    if (ds_here && !r->n && !g_rt.ui_seen) { g_rt.ui_seen = 1; g_rt.filt = 0; return; }   // The first render pass in this frame with the main depth and no color = the UI stencil pass: run as usual
    g_rt.hdrp = hdr_here && ds_here && r->n == 1 && !g_rt.mainp_seen; if (g_rt.hdrp) g_rt.mainp_seen = 1;   // The first render pass in this frame with "only opaque HDR + main depth attached" = the main pass
    g_rt.filt = g_rt.shadow || (ds_here && !r->n) || g_rt.hdrp;
    if (g_frz_state == 2 && g_rt.mainp_seen && !g_rt.hdrp) {   // Extrapolated frame, after the main pass: post-processing runs only in the unit / effect region (plan B)
        int c0ldr = r->n >= 1 && r->color[0] && g_rt.ldr_ser && r->color[0]->serial == g_rt.ldr_ser, ovr = 0; g_rt.skipall = 0;
        if (g_rt.fx_state == 2) {}   // After FXAA (sharpening upscale + vignette, UI compositing): run everything
        else if (g_rt.fx_state == 1) g_rt.fx_state = 2;
        else if (!g_rt.oit_seen && ds_here && r->n >= 2) { g_rt.oit_seen = 1; ovr = 3; }   // Translucent (weighted blended, two targets): draw as usual, writing effect bit 0x40 where it passes
        else if (!g_rt.comp_seen) { if (ds_here && c0ldr) { g_rt.comp_seen = 1; ovr = 1; rt_blit_shift(g_rt.pl469, g_rt.ldr, g_rt.sdx, g_rt.sdy); } else g_rt.skipall = 1; }   // Before compositing: the previous frame's post-FXAA 469 is copied in with an integer shift (then recomputed in the region)   // the other passes before compositing (mip chain, 511 refraction layer etc.) are skipped
        else if (ds_here && c0ldr && g_rt.tm_seen) {   // The FXAA pass itself (recomputed in the region; the 469 outside the region was already shift-copied before compositing)
            g_rt.fx_state = 1; ovr = 1; }
        else if (ds_here) { if (hdr_here && r->n == 1) { g_rt.tm_seen = 1; rt_blit_shift(g_rt.pl468, g_rt.hdr, g_rt.sdx, g_rt.sdy); } ovr = 1; }   // Before tone mapping: the previous frame's post-tone-mapping 468 (LDR) is copied in with an integer shift   // add bloom, add fog, tone mapping: run only in the region
        else g_rt.skipall = 1;   // Those without the main depth attached, such as the bloom chain: skip (the region keeps using the previous frame's bloom)
        if (ovr != g_rt_ovr) { g_rt_ovr = ovr; g_dss_dirty = 1; }
    }
}
static int rt_op(unsigned op) {   // Non-set-target operations in an extrapolated frame: returns 1 = not executed
    if (g_frz_state == 2 && g_rt_ovr == 1 && op == OP_CLEAR_COLOR) return 1;   // The region post-processing passes do not clear: outside the region is the previous frame's result copied in with a shift
    if (g_rt.skipall && (op == OP_DRAW || op == OP_DRAW_IDX || op == OP_CLEAR_COLOR || op == OP_CLEAR_DS || op == OP_COPY_T2T || op == OP_CLEAR_TEX || op == OP_DISPATCH)) return 1;
    if (op == OP_DRAW || op == OP_DRAW_IDX) {
        if (g_frz_state == 2 && !g_rt.cam_checked) {   // First draw of an extrapolated frame (UI stencil pass, before shadows / prepass): check this frame's camera, and on a jump switch the whole frame to fully drawn
            g_rt.cam_checked = 1; const p_bind_buf *u = &S.ubo[0][3]; void *mb; uint64_t mo; const uint8_t *base; int ok = 0;
            if (u->addr && u->size >= 528 && mtl_addr(u->addr, u->size, &mb, &mo) && (base = [(__bridge id<MTLBuffer>)mb contents])) {
                const float *cur = (const float *)(base + mo + 336), *prev = (const float *)(base + mo + 464); int dx, dy; float md = 0;
                for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) md = MAX(md, fabsf(cur[r * 4 + c] - prev[r * 4 + c]));   // The first three rows (orientation / scale) should be unchanged: an orthographic camera only translates
                rt_shift_px(prev, cur, g_rt.hdr.width, g_rt.hdr.height, &dx, &dy);
                ok = md < 1e-4f && abs(dx) <= 64 && abs(dy) <= 64; if (!ok) { static int n; if (n++ < 20) MLOG("运行时外推：镜头跳变（平移 %d,%d 像素、朝向差 %g），本帧改全画", dx, dy, md); } }
            if (!ok) { g_frz_state = 0; g_rt.n_jump++; rt_frame_begin(); g_rt.cam_checked = 1; return 0; }
        }
        if (g_frz_state == 0) { if (g_rt.cur_sh >= 0) { if (rt_is_skin()) g_rt.sh[g_rt.cur_sh].skin++; else g_rt.sh[g_rt.cur_sh].other++; } return 0; }
        if (!g_rt.filt) return 0;
        if (!rt_is_skin()) return 1;
        if (!g_rt.ex_done && !g_rt.shadow) {   // Before the first skinned draw of the depth prepass: the extrapolation pass (reads the cb3 bound by this draw: +336 this frame's, +464 the previous frame's view-projection)
            g_rt.ex_done = 1; const p_bind_buf *u = &S.ubo[0][3]; void *mb; uint64_t mo; const uint8_t *base;
            if (u->addr && u->size >= 528 && mtl_addr(u->addr, u->size, &mb, &mo) && (base = [(__bridge id<MTLBuffer>)mb contents])) {
                float cur[16], prev[16]; memcpy(cur, base + mo + 336, 64); memcpy(prev, base + mo + 464, 64); memcpy(g_rt.vpc, cur, 64); memcpy(g_rt.vpp, prev, 64); rt_shift_px(prev, cur, g_rt.hdr.width, g_rt.hdr.height, &g_rt.sdx, &g_rt.sdy);
                rp_end(); if (rt_ex_encode(g_rt.hdr, g_rt.ds, g_rt.phdr, g_rt.pds, prev, cur, 0x10, 0x2)) g_rt.n_ex++; else g_rt.n_noex++;
            } else g_rt.n_noex++;
        }
        return 0;
    }
    if (g_frz_state == 2 && g_rt.filt) {
        if (op == OP_CLEAR_COLOR || op == OP_CLEAR_DS) {
            if (g_rt.shadow && g_rt.cur_sh >= 0 && !g_rt.sh[g_rt.cur_sh].other) return 0;   // A shadow level that had only skinned draws in the previous fully drawn frame (character-only level): clear and draw as usual
            return 1; }
    }
    return 0;
}
static int frz_op(unsigned op, const void *a) {   // Returns 1 = this operation is not executed
    if (op == OP_SET_RT) {
        const p_set_rt *r = a; int win = 0;
        for (int i = 0; i < r->n; i++) { uint64_t s_; int lv, sl; rt_sub(r, i, &s_, &lv, &sl); if (s_ && xv_present(s_)) win = 1; }
        if (g_frz_phase == 2 && !win && rt_want()) {   // Runtime extrapolation: new frame, fully drawn frames and extrapolated frames alternate
            int was = g_frz_state; g_frz_phase = 0; g_rt.was_on = 1;
            { static int dbg = -1, n; if (dbg < 0) dbg = getenv("DM_RT_DBG") != NULL; if (dbg && n < 4000 && g_rt.ds_ser) { n++; MLOG("[rtdbg] 帧界 通道 %llu 上一段状态 %d 认出主通道 %d 拷了 %d 外推 %d fx %d ldr %d hoist %d fz %d defer %d/%d", g_stat_pass, was, g_rt.main_seen, g_rt.copied, g_rt.ex_done, g_rt.fx_state, g_rt.have_ldr, g_hoist_n, g_fz.active, defer_has_active(), g_defer_replaying); } }
            if (g_rt.force_full > 0) g_rt.force_full--;
            g_frz_state = !was && g_rt.have_prev && g_rt.have_ldr && !g_rt.force_full && !g_hoist_n && !g_fz.active && !defer_has_active() && !g_defer_replaying ? 2 : 0;
            if (g_frz_state == 2) g_rt.frame++; else g_rt.n_full++;
            if (g_frz_state != was && (g_rt.frame + g_rt.n_full) % 600 < 2) MLOG("运行时外推：全画帧 %llu、外推帧 %d（外推趟编了 %llu 次、没编 %llu 次；镜头跳变改全画 %llu 次）", g_rt.n_full, g_rt.frame, g_rt.n_ex, g_rt.n_noex, g_rt.n_jump);
            rt_frame_begin();
        }
        if (g_frz_phase != 2 && rt_want()) { if (win) g_frz_phase = 2; rt_set_rt(r, win); return g_rt.skipall; }
        if (g_frz_phase == 2 && win && rt_want()) { rt_set_rt(r, win); return 0; }
        { int on = rt_want(); if (g_rt.was_on && !on) {   // The new rendering method was just turned off (version file fallback / panel freeze takes over / fallback for the whole run): reset state to zero and hand back to the original path
              g_frz_state = 0; g_frz_phase = 2; g_rt.have_prev = g_rt.have_ldr = 0; if (g_rt_ovr) { g_rt_ovr = 0; g_dss_dirty = 1; } g_rt.skipall = 0;
              MLOG("运行时外推：新画法停用（画法 %d、世界冻结 %d、整局退回 %d）", g_dm_rt_ab, g_frz_on, g_rt.disabled); }
          g_rt.was_on = on; }
        if (g_frz_phase == 2 && !win) {   // New frame
            int on = frz_wanted(), was = g_frz_state; g_frz_phase = 0; g_frz_seen_ds = 0;
            { static uint64_t fr, p0, k0; if (was == 2 && (++fr == 4 || fr % 600 == 0)) MLOG("世界冻结：第 %llu 个冻结帧开了 %llu 个通道、跳过 %llu 条设目标", fr, g_stat_pass - p0, g_frz_skip_n - k0); if (was != 2) fr = 0; p0 = g_stat_pass; k0 = g_frz_skip_n; }
            if (!on) g_frz_state = 0;
            else if (was) g_frz_state = 2;
            else if (!g_hoist_n && !g_fz.active && !defer_has_active() && !g_defer_replaying) g_frz_state = 1;   // The premises / merges / deferrals left by the previous frame have not finished yet: enter next frame
            if (g_frz_state != was) MLOG("世界冻结：通道 %llu 起 %s（完整帧累计 %llu，已跳过通道 %llu）", g_stat_pass + 1, g_frz_state == 1 ? "完整帧（不带界面模板）" : g_frz_state == 2 ? "冻结帧（只画界面与末尾合成）" : "恢复正常", g_frz_full_n += g_frz_state == 1, g_frz_skip_n);
        }
        g_frz_drop = 0;
        if (win) { g_frz_phase = 2; return 0; }
        if (!g_frz_state || g_frz_phase == 2) return 0;
        if (g_frz_phase == 0 && !r->n && r->depth && !g_frz_seen_ds) {   // The first render pass without color in this frame
            g_frz_seen_ds = 1;
            if (g_frz_state == 2) g_frz_phase = 1;
            else { uint64_t s_; int lv, sl; rt_sub(r, 8, &s_, &lv, &sl); static int neg = -1; if (neg < 0) neg = getenv("DM_NVN_TEST_FRZ_NEG") != NULL;   // Negative control: the full frame is drawn with the UI stencil as usual, and old pixels must be exposed when the panel shrinks
                   g_frz_drop = !neg && MAX(r->depth->d.width >> lv, 1u) != MAX(r->depth->d.height >> lv, 1u); }   // Full frame: the one that is not square (shadow map) is the UI stencil pass
        }
        if (g_frz_phase == 1) { g_frz_skip_n++; return 1; }
        return 0;
    }
    if (rt_want() && g_frz_phase != 2) return rt_op(op);
    if (g_frz_drop) return op == OP_DRAW || op == OP_DRAW_IDX;
    if (g_frz_state == 2 && g_frz_phase == 1) return op == OP_CLEAR_COLOR || op == OP_CLEAR_DS || op == OP_DRAW || op == OP_DRAW_IDX || op == OP_COPY_T2T || op == OP_CLEAR_TEX || op == OP_DISPATCH;
    return 0;
}

// Hand a submission to the GPU in segments (DM_NVN_CUT=draw count, 0 = off; default 64): when the game holds back the next frame's submit until this frame's GPU work is done (The Legend of Zelda: Breath of the Wild: the main thread's nvnSyncWait on the fence of the frame just submitted),
// the GPU sits idle for the milliseconds the host spends translating inside the submit (in the shrine each frame is GPU 13 ms + translation 7 ms back to back). Each time this many draws have accumulated, the command buffer encoded so far is committed between two passes: the GPU runs the earlier passes while the CPU translates the later ones.
// Cut only when no encoder is open and no pass is deferred / merged / hoisted; commit this batch's uploads first (later draws use them). The fence signal and completion work stay on the last segment only: the queue executes in commit order and every encoder starts with a queue barrier
static int cut_draws(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_CUT"); v = e ? atoi(e) : 64; MLOG("分段提交：每 %d 次绘制在通道之间先交一段（DM_NVN_CUT，0 关）", v); } return v; }
static uint64_t g_cut_draw0, g_cut_sub, g_cuts;
static void sub_cut(void) {
    int n = cut_draws();
    if (n <= 0 || !g_cb || g_enc) return;
    if (g_cut_sub != g_sub_serial) { g_cut_sub = g_sub_serial; g_cut_draw0 = g_stat_draw; }   // count from the start of each submission
    if (g_stat_draw - g_cut_draw0 < (uint64_t)n || g_hoist_n || g_defer_nact || g_defer_pend || g_fz.active || g_rt_merged) return;
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    up_flush();
    uint64_t f = atomic_load(&g_cur_frame); gpu_tag_begin(f);
    NSArray<NvDeferredRelease *> *cut_retire = defer_take_cut_batch();

    [g_cb addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        log_function_errors(cb, "split", f);
        gpu_tag_done(f, cb.GPUStartTime, cb.GPUEndTime);
        if (cb.error) { atomic_store(&g_last_commit_error, 1); static int k; if (k++ < 8) MLOG("【红】分段命令缓冲执行出错：%s", cb.error.description.UTF8String); }
        defer_release_batch(cut_retire);
        res_commit();
    }];
    res_commit();
    [g_cb commit];
    atomic_fetch_add_explicit(&g_cut_committed_diag, 1, memory_order_relaxed);
    g_cb = replay_cb();
    g_cut_draw0 = g_stat_draw;
    if (!g_cuts++) MLOG("分段提交：首段在帧 %llu 通道 %llu 交出", (unsigned long long)f, (unsigned long long)g_stat_pass);
}
uint64_t mtl_cut_count(void) { return g_cuts; }

// ---------- Clears that a load action cannot express ----------
// A mid-pass clear, a partial channel mask, or a scissored clear is rendered as
// one full-pass triangle. This preserves the open pass and changes only the
// requested attachment channels inside the active scissor.
static int pf_int_kind(MTLPixelFormat f) {
    switch (f) {
        case MTLPixelFormatR8Uint: case MTLPixelFormatR16Uint: case MTLPixelFormatR32Uint: case MTLPixelFormatRG8Uint: case MTLPixelFormatRG16Uint: case MTLPixelFormatRG32Uint:
        case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGB10A2Uint: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA32Uint: return 1;
        case MTLPixelFormatR8Sint: case MTLPixelFormatR16Sint: case MTLPixelFormatR32Sint: case MTLPixelFormatRG8Sint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRG32Sint:
        case MTLPixelFormatRGBA8Sint: case MTLPixelFormatRGBA16Sint: case MTLPixelFormatRGBA32Sint: return 2;
        default: return 0;
    }
}
static id<MTLLibrary> clr_lib(int slot, int kind) {
    static id<MTLLibrary> libs[8][3]; static uint8_t failed[8][3];
    if (libs[slot][kind] || failed[slot][kind]) return libs[slot][kind];
    static const char *ty[3] = {"float", "uint", "int"}; char src[720];
    snprintf(src, sizeof src, "#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\n"
             "vertex V clr_vs(uint i [[vertex_id]], device const float *z [[buffer(0)]]) { V o; o.p = float4(float2(float((i << 1) & 2), float(i & 2)) * 2.0 - 1.0, z[0], 1.0); return o; }\n"
             "struct O { %s4 c [[color(%d)]]; };\nfragment O clr_fs(device const %s4 *v [[buffer(0)]]) { O o; o.c = v[0]; return o; }\n", ty[kind], slot, ty[kind]);
    NSError *e = nil; libs[slot][kind] = [g_dev newLibraryWithSource:@(src) options:nil error:&e];
    if (!libs[slot][kind]) { failed[slot][kind] = 1; MLOG("[RED] clear-triangle shader compile failed (slot %d kind %d): %s", slot, kind, e.localizedDescription.UTF8String); }
    return libs[slot][kind];
}
typedef struct { int32_t slot, kind, mask; uint32_t cfmt[8], dfmt, sfmt, samples; } clr_key;
static id<MTLRenderPipelineState> clr_pso(const clr_key *k) {
    static NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *cache; if (!cache) cache = [NSMutableDictionary dictionary];
    NSData *kd = [NSData dataWithBytes:k length:sizeof *k]; id<MTLRenderPipelineState> ps = cache[kd]; if (ps) return ps;
    id<MTLLibrary> lib = clr_lib(k->slot < 0 ? 0 : k->slot, k->slot < 0 ? 0 : k->kind); if (!lib) return nil;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"clr_vs"]; if (k->slot >= 0) d.fragmentFunction = [lib newFunctionWithName:@"clr_fs"];
    for (int i = 0; i < 8; i++) if (k->cfmt[i]) { d.colorAttachments[i].pixelFormat = (MTLPixelFormat)k->cfmt[i]; d.colorAttachments[i].writeMask = MTLColorWriteMaskNone; }
    if (k->slot >= 0) d.colorAttachments[k->slot].writeMask = (MTLColorWriteMask)((k->mask & 1 ? MTLColorWriteMaskRed : 0) | (k->mask & 2 ? MTLColorWriteMaskGreen : 0) | (k->mask & 4 ? MTLColorWriteMaskBlue : 0) | (k->mask & 8 ? MTLColorWriteMaskAlpha : 0));
    d.depthAttachmentPixelFormat = (MTLPixelFormat)k->dfmt; d.stencilAttachmentPixelFormat = (MTLPixelFormat)k->sfmt; d.rasterSampleCount = k->samples ? k->samples : 1;
    NSError *e = nil; ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!ps) { static int n; if (n++ < 4) MLOG("[RED] cannot create clear-triangle pipeline (slot %d color format %u depth %u stencil %u samples %u): %s", k->slot, k->cfmt[k->slot], k->dfmt, k->sfmt, k->samples, e.localizedDescription.UTF8String); return nil; }
    cache[kd] = ps; return ps;
}
static id<MTLDepthStencilState> clr_dss(int dw, unsigned sm) {
    static id<MTLDepthStencilState> cache[2][256]; if (cache[dw][sm]) return cache[dw][sm];
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new]; d.depthCompareFunction = MTLCompareFunctionAlways; d.depthWriteEnabled = dw != 0;
    if (sm) {
        MTLStencilDescriptor *st = [MTLStencilDescriptor new]; st.stencilCompareFunction = MTLCompareFunctionAlways;
        st.stencilFailureOperation = st.depthFailureOperation = MTLStencilOperationKeep; st.depthStencilPassOperation = MTLStencilOperationReplace;
        st.readMask = 0xff; st.writeMask = sm; d.frontFaceStencil = d.backFaceStencil = st;
    }
    return cache[dw][sm] = [g_dev newDepthStencilStateWithDescriptor:d];
}
static int clr_scissor(MTLScissorRect *sc) {
    NSUInteger tw = g_pass.tw, th = g_pass.th; *sc = (MTLScissorRect){0, 0, tw, th};
    if (!R.has_sc) return tw && th;
    long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
    if (x1 > (long)tw) x1 = (long)tw; if (y1 > (long)th) y1 = (long)th;
    if (x1 <= x0 || y1 <= y0) return 0;
    *sc = (MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)}; return 1;
}
static void clr_tri(id<MTLRenderCommandEncoder> en, id<MTLRenderPipelineState> ps, id<MTLDepthStencilState> ds, MTLScissorRect sc, const uint32_t val[4], float z, uint32_t sref) {
    id<MTLBuffer> b; size_t o = arena_alloc(32, &b); uint8_t *p = (uint8_t *)b.contents + o; memcpy(p, val, 16); memset(p + 16, 0, 16); memcpy(p + 16, &z, 4);
    legacy_bind_buffer(en, 0, 0, b, o + 16); legacy_bind_buffer(en, 1, 0, b, o);
    enc_r_pso(en, ps); g_enc_pso = nil; enc_r_dss(en, ds); g_enc_dss = nil;
    if (g_enc_vis) { enc_r_visibility(en, MTLVisibilityResultModeDisabled, 0); g_enc_vis = 0; }
    enc_r_viewport(en, (MTLViewport){0, 0, (double)g_pass.tw, (double)g_pass.th, 0, 1}); enc_r_scissor(en, sc);
    enc_r_cull(en, MTLCullModeNone); enc_r_fill(en, MTLTriangleFillModeFill); enc_r_depth_bias(en, 0, 0, 0); enc_r_depth_clip(en, YES); enc_r_stencil_ref(en, sref, sref); g_ed.ok = 0;
    enc_r_draw(en, MTLPrimitiveTypeTriangle, 0, 3);
}
static void clr_note(int slot, int depth, int stencil) {
    if (slot >= 0 && R.rt.color[slot]) { zskip_del(R.rt.color[slot]->serial); g_zskip_cand_ok[slot] = 0; }
    for (int i = 0; i < g_nsd; i++) { sd_att *t = &g_sd[i];
        if ((t->depth == 0 && slot >= 0 && t->idx == slot) || (t->depth == 1 && depth) || (t->depth == 2 && stencil)) { t->wrote = 1; t->wwhy = 2; } }
}
static void clear_color_tri(const p_clear_color *c) {
    int slot = c->index, mask = c->mask & 0xf;
    if (!mask || slot < 0 || slot >= 8 || slot >= R.rt.n || !R.rt.color[slot]) return;
    NSUInteger slices = !g_slice_replaying ? slice_target_count() : 1;
    if (slices > 1) {
        rp_end();
        typeof(R) saved = R; g_slice_replaying = 1;
        for (uint32_t slice = 0; slice < slices; slice++) {
            R = saved; slice_select(slice);
            clear_color_tri(c);
            // Pending full clears still cover every slice even when this
            // masked triangle has an empty scissor or cannot draw.
            rp_flush();
        }
        R = saved; memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
        g_slice_replaying = 0; g_selected_slice = 0; args_dirty_all(); g_pso_dirty = 1;
        return;
    }
    id<MTLRenderCommandEncoder> en = rp_begin();
    if (en && !g_pass.cfmt[slot] && g_enc_mode) { rp_end(); en = rp_begin(); }
    if (!en) return;
    int at = slot;
    if (!g_pass.cfmt[at] && rt_dup_slot(&R.rt, slot))
        for (int j = 0; j < slot; j++) { const tex_t *a = R.rt.color[j], *b = R.rt.color[slot]; if (!a || a->serial != b->serial || !g_pass.cfmt[j]) continue;
            const view_t *va = &R.rt.cview[j], *vb = &R.rt.cview[slot]; int ha = R.rt.has_cview[j], hb = R.rt.has_cview[slot];
            if ((ha && va->set_levels ? va->base_level : 0) == (hb && vb->set_levels ? vb->base_level : 0) && (ha && va->set_layers ? va->min_layer : 0) == (hb && vb->set_layers ? vb->min_layer : 0)) { at = j; break; } }
    if (!g_pass.cfmt[at]) { PD("c %llu color-clear triangle skipped: slot %d is not in this pass", g_stat_pass, slot); return; }
    MTLScissorRect sc; if (!clr_scissor(&sc)) { PD("c %llu color-clear triangle skipped: empty scissor slot %d", g_stat_pass, slot); return; }
    int kind = pf_int_kind((MTLPixelFormat)g_pass.cfmt[at]);
    clr_key k; memset(&k, 0, sizeof k); k.slot = at; k.kind = kind; k.mask = mask; memcpy(k.cfmt, g_pass.cfmt, sizeof k.cfmt); k.dfmt = g_pass.dfmt; k.sfmt = g_pass.sfmt; k.samples = g_pass.samples;
    id<MTLRenderPipelineState> ps = clr_pso(&k); id<MTLDepthStencilState> ds = clr_dss(0, 0); if (!ps || !ds) return;
    uint32_t v[4];
    for (int i = 0; i < 4; i++) { float f = c->rgba[i]; int32_t si;
        if (kind == 1) v[i] = f > 0 ? (f < 4294967040.f ? (uint32_t)f : 0xffffffffu) : 0;
        else if (kind == 2) { si = !(f == f) ? 0 : f >= 2147483520.f ? INT32_MAX : f <= -2147483648.f ? INT32_MIN : (int32_t)f; memcpy(&v[i], &si, 4); }
        else memcpy(&v[i], &f, 4); }
    clr_tri(en, ps, ds, sc, v, 0, 0); clr_note(at, 0, 0);
    static uint64_t n; if (!n++) MLOG("mid-pass / partial-mask / partial-scissor color clear: drawn as a full-pass triangle (first case pass %llu slot %d mask %#x scissor %lu,%lu %lux%lu)", g_stat_pass, at, mask,
        (unsigned long)sc.x, (unsigned long)sc.y, (unsigned long)sc.width, (unsigned long)sc.height);
    PD("c %llu color clear drawn as triangle slot%d ser=%llu mask=%#x scissor=%lu,%lu,%lu,%lu", g_stat_pass, at, (unsigned long long)R.rt.color[at]->serial, mask,
       (unsigned long)sc.x, (unsigned long)sc.y, (unsigned long)sc.width, (unsigned long)sc.height);
}
static void clear_ds_tri(const p_clear_ds *c) {
    unsigned want_s = (unsigned)c->stencil_mask & 0xff;
    if (!c->depth_mask && !want_s) return;
    id<MTLRenderCommandEncoder> en = rp_begin();
    if (en && g_enc_mode && ((c->depth_mask && !g_pass.hd) || (want_s && !g_pass.hs))) { rp_end(); en = rp_begin(); }
    if (!en) return;
    int dw = c->depth_mask && g_pass.hd; unsigned sm = g_pass.hs ? want_s : 0;
    if (!dw && !sm) return;
    MTLScissorRect sc; if (!clr_scissor(&sc)) { PD("c %llu depth-stencil clear triangle skipped: empty scissor", g_stat_pass); return; }
    clr_key k; memset(&k, 0, sizeof k); k.slot = -1; memcpy(k.cfmt, g_pass.cfmt, sizeof k.cfmt); k.dfmt = g_pass.dfmt; k.sfmt = g_pass.sfmt; k.samples = g_pass.samples;
    id<MTLRenderPipelineState> ps = clr_pso(&k); id<MTLDepthStencilState> ds = clr_dss(dw, sm); if (!ps || !ds) return;
    float z = c->depth > 0 ? (c->depth < 1 ? c->depth : 1) : 0;
    const uint32_t v[4] = {0}; clr_tri(en, ps, ds, sc, v, z, (uint32_t)c->stencil & 0xff); clr_note(-1, dw, sm != 0);
    static uint64_t n; if (!n++) MLOG("mid-pass scissored depth-stencil clear: drawn as a full-pass triangle (first case pass %llu depth %d stencil mask %#x scissor %lu,%lu %lux%lu)", g_stat_pass, dw, sm,
        (unsigned long)sc.x, (unsigned long)sc.y, (unsigned long)sc.width, (unsigned long)sc.height);
    PD("c %llu depth-stencil clear drawn as triangle ser=%llu depth=%g(%d) stencil=%d mask=%#x scissor=%lu,%lu,%lu,%lu", g_stat_pass,
       R.rt.depth ? (unsigned long long)R.rt.depth->serial : 0ull, z, dw, c->stencil, sm, (unsigned long)sc.x, (unsigned long)sc.y, (unsigned long)sc.width, (unsigned long)sc.height);
}

void mtl_exec(const uint8_t *p) {
    static int inited; if (!inited) { inited = 1; state_reset(); }
    static int dump_all = -1; if (dump_all < 0) dump_all = getenv("DM_NVN_PASSDUMP_ALL") != NULL;
    int trace = dump_all && passdump();   // Normal mode does not check the frame range per operation; the full command position is only used for the designated diagnostic frames
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        g_exec_cur = p;   // Scan start point of the store lookahead: the operation that ends the render pass (inclusive)
        if (p == g_defer_pend) { for (int gi = 0; gi < 4; gi++) if (g_defer_slots[gi].active && g_defer_slots[gi].p_end_op == p) g_defer_slots[gi].in_p = 0; g_defer_pend = NULL; }
        sdx_seq(p);
        if (h.op == OP_SET_RT) rt_toggle_poll();
        if ((g_frz_on || g_frz_state || g_dm_rt_ab == 1) && frz_op(h.op, p + 8)) { p += h.size; continue; }
        if (trace) PD("o cmd=%p pass=%llu op=%u size=%u", p, g_stat_pass, (unsigned)h.op, (unsigned)h.size);
        // Diagnostic DM_NVN_ALLOCPROF=1: growth of the GPU allocated size is charged to the previous operation's type (rough: allocations on other threads are counted too); the memory report line appends the top 4
        if (g_allocprof < 0) g_allocprof = getenv("DM_NVN_ALLOCPROF") != NULL;
        if (g_allocprof) { static uint64_t last; static int prev = -1; uint64_t now = g_dev.currentAllocatedSize; if (prev >= 0 && prev < 64 && now > last) g_ap_bytes[prev] += now - last; last = now; prev = h.op; }
        if (g_hoist_n && hoist_take(p)) { if (g_fz.active && p == g_fz.rt2_op) { g_fz.phase = 2; g_rt_merged = 0; PD("F %llu 合并通道进入后一个通道的绘制", g_stat_pass); } p += h.size; continue; }   // A pure-clear render pass already executed early / the start of a later pass already merged in   // Entering the later pass: the same-target continuation of the earlier pass ends here; the later pass reading the earlier pass's attachment is proven to be same-pixel by fz_p2_draw, and is no longer split by the continuation rule
        const void *a = p + 8;
        switch (h.op) {
            case OP_END: g_exec_cur = NULL;
                if (g_hoist_n && !g_ret_n && !g_nrest) {
                    if (g_fz.active) fz_unskip();
                    if (g_hoist_n) { MLOG("【红】纯清除通道前提：提交结束时还有 %d 条已前提的操作没走到，清空（不应发生）", g_hoist_n); g_hoist_n = 0; }
                }
                return;   // An end-of-pass triggered outside a list (present / CPU readback etc.) must not scan expired command memory: a null start point means store everything
            case OP_BEGIN: break;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8);
                if (g_ret_n < 16) g_ret[g_ret_n] = p + h.size; g_ret_n++;   // Return stack for lookahead: after a sub-list ends, continue looking at the caller
                mtl_exec((const uint8_t *)t); g_ret_n--; break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, (const uint8_t *)a + 8, 4); memcpy(&v, (const uint8_t *)a + 12, 4);
                PD("Y signal_event cmd=%p dst=%p mode=%u value=%u", p, val, m, v); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, (const uint8_t *)a + 8, 8); g_cnt_report[t & 31]++;
                PD("Q report type=%llu cmd=%p dst=%#llx", (unsigned long long)t, p, (unsigned long long)ad);
                if (DM_GAME_RENDER_COND) rc_note(ad, t == 0 || t == 0x10 ? 1 : 2);
                if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) { post((post_op){3, NULL, 0, 0, ad, (uint32_t)t, vis_snapshot()}); g_vis_cur = -1; }
                else mtl_sub_counter(ad, (uint32_t)t);
                break; }
            case OP_RENDER_COND: { const p_render_cond *rc = a;
                if (rc->mode >= 2) {
                    int would_skip = rc->mode == 3;
                    rc_override_note(rc, would_skip, 1, 0, 0);
                    g_rc_skip = would_skip && !rc_always_draw();
                    PD("E %llu render enable %d", g_stat_pass, rc->mode == 2);
                } else {
                    uint64_t x = 0, y = 0;
                    int known = rc_value(rc->addr, &x) & rc_value(rc->addr + 16, &y);
                    int would_skip = known && (rc->mode ? x == y : x != y);
                    rc_override_note(rc, would_skip, known, x, y);
                    g_rc_skip = would_skip && !rc_always_draw();
                    PD("E %llu conditional render %s addr=%#llx values %llu / %llu%s%s", g_stat_pass,
                       rc->mode ? "draw if not equal" : "draw if equal", (unsigned long long)rc->addr,
                       (unsigned long long)x, (unsigned long long)y, known ? "" : " (unknown, drawn)",
                       g_rc_skip ? " -> not drawn" : "");
                }
                break; }
            case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); g_cnt_reset[t & 31]++; PD("Q reset type=%llu cmd=%p", (unsigned long long)t, p);
                if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
#ifdef NVN_RT_RANGE_TEST
                    rr_reset();
#endif
                    g_vis_on = 1; g_vis_counting = 1; vis_clear(); }
                break; }
            case OP_CLEAR_BUF: { const p_clear_buf *cb = a; mtl_sub_fill(cb->addr, cb->size, cb->value); break; }
            case OP_COPY_B2B: { uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, (const uint8_t *)a + 8, 8); memcpy(&n, (const uint8_t *)a + 16, 8);
                PD("Y copy_b2b cmd=%p src=%#llx dst=%#llx bytes=%llu", p, (unsigned long long)s_, (unsigned long long)d, (unsigned long long)n); mtl_sub_copy(s_, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8);
                PD("Y update_ub cmd=%p src=%#llx dst=%#llx bytes=%llu f=%g,%g,%g,%g", p, (unsigned long long)((const uint8_t *)a + 16), (unsigned long long)d, (unsigned long long)n,
                   n >= 4 ? ((const float *)((const uint8_t *)a + 16))[0] : 0.f, n >= 8 ? ((const float *)((const uint8_t *)a + 16))[1] : 0.f, n >= 12 ? ((const float *)((const uint8_t *)a + 16))[2] : 0.f, n >= 16 ? ((const float *)((const uint8_t *)a + 16))[3] : 0.f); mtl_sub_update((const uint8_t *)a + 16, d, n); break; }
            case OP_SET_RT: {
                const p_set_rt *r = a;
                if (defer_begin_q(p)) break;
                int pend = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
                if (g_enc && !pend && R.has_rt && tbdr_on() && rt_range_same(&R.rt,r)) {
                    g_rt_merged = g_rt_range_active = g_rt_range_pending = 1; R.rt = *r; break; }
                if (g_enc && g_fz.active && g_fz.phase == 1 && fz_reuse_op(p) && !pend && g_enc_mode != 1 && R.has_rt && tbdr_on() && rt_same(&g_fz.rt1, r)) {
                    g_rt_merged = 1; if (!g_rt_merge_n++) MLOG("同目标续用：首例 通道 %llu", g_stat_pass); PD("M %llu 同目标续用通道", g_stat_pass); break; }
                // Do not continue a render pass opened only for a clear; one with fewer depth / stencil attachments can be continued (check the planes before drawing)
                if (g_enc && g_enc_mode != 1 && !pend && R.has_rt && tbdr_on() && rt_same(&R.rt, r)) { g_rt_merged = 1; if (!g_rt_merge_n++) MLOG("同目标续用：首例 通道 %llu", g_stat_pass); PD("M %llu 同目标续用通道", g_stat_pass); R.rt = *r; break; }
                rp_flush(); sub_cut(); R.rt = *r; R.has_rt = 1; break;
            }
            case OP_CLEAR_COLOR: {
                const p_clear_color *c = a;
                if (g_rc_skip) { PD("c %llu color clear skipped by conditional rendering slot%d", g_stat_pass, c->index); break; }
                // A scissored clear to 0 outside a pass with all channels (SMBW clears pyramid levels as sub-rectangles of 480×272 and other targets): copy from the zero buffer into the scissor rectangle in the replay stream
                if (!g_enc && c->mask == 0xf && !c->rgba[0] && !c->rgba[1] && !c->rgba[2] && !c->rgba[3] && R.has_sc && c->index >= 0 && c->index < R.rt.n && R.rt.color[c->index]
                    && !scissor_full(R.rt.color[c->index], &R.rt.cview[c->index], R.rt.has_cview[c->index]) && clear_rect_zero(c->index)) break;
                if (g_rt_merged) rp_end();   // A clear arrived in a continued render pass: end it here, and the clear is attached to the start of the next render pass as usual
                if (c->index < 0 || c->index >= R.rt.n || !R.rt.color[c->index]) { RED_ONCE(5, "清色目标 %d 无效", c->index); break; }
                if (g_enc || c->mask != 0xf || !scissor_full(R.rt.color[c->index], &R.rt.cview[c->index], R.rt.has_cview[c->index])) {
                    clear_color_tri(c);
                    break; }
                R.clear_c[c->index] = 1; R.cc[c->index] = MTLClearColorMake(c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]);
                break;
            }
            case OP_CLEAR_DS: {
                const p_clear_ds *c = a;
                if (g_rc_skip) { PD("c %llu depth-stencil clear skipped by conditional rendering", g_stat_pass); break; }
                if (g_rt_merged) rp_end();
                // Depth / stencil clear in the middle of a pass: end the current pass and attach the clear to the start of the next one (color attachments load as usual, as a continued pass does above when a clear arrives). It used to be skipped, and later draws depth-tested against stale depth.
                // A partial clear cannot be represented by a Metal load action, so keep the pass open and draw it through clear_ds_tri.
                if (g_enc) {
                    if (R.rt.depth && !scissor_full(R.rt.depth, &R.rt.dview, R.rt.has_dview)) { clear_ds_tri(c); break; }
                    { static int once; if (!once++) MLOG("通道中途的深度模板清除：结束本通道，清除挂到下一个通道开头（首例 通道 %llu）", g_stat_pass); }
                    rp_end();
                }
                if (c->depth_mask) { R.clear_d = 1; R.cd = c->depth; }
                if (c->stencil_mask) { R.clear_s = 1; R.cs = (uint32_t)c->stencil; }
                break;
            }
            case OP_DRAW: { if (g_rc_skip) break; p_draw tmp; const p_draw *d = draw_rec(a, h.size, &tmp);
                g_exec_next = p + h.size; draw(d, NULL); g_exec_next = NULL; break; }   // Lookahead start point; compact direct draws omit the indirect fields
            case OP_DRAW_IDX: { if (g_rc_skip) break; p_draw_idx tmp; const p_draw_idx *d = draw_idx_rec(a, h.size, &tmp);
                g_exec_next = p + h.size; draw(NULL, d); g_exec_next = NULL; break; }
            // Do not mark dirty if the state did not change (the game often re-sends the same program / state block / vertex format before each draw; the dirty flag only decides whether to re-fetch the pipeline / depth-stencil object, and equal values mean the same object)
            case OP_SCISSOR: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
            case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL:
            case OP_POLY_OFFSET: case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_DEPTH_CLAMP: case OP_BLEND_COLOR: case OP_COPY_STRIDE:
                state_op(h.op, a); break;
            case OP_TF_BIND: { const p_tf_bind *b = a; PD("Y tf bind %d addr=%#llx size=%llu", b->index, (unsigned long long)b->addr, (unsigned long long)b->size); state_op(h.op, a); break; }
            case OP_TF_STATE: { const p_tf_state *b = a; static const char *const what[4] = {"begin", "end", "pause", "resume"}; PD("Y tf %s written %llu/%llu/%llu/%llu", what[b->what & 3],
                (unsigned long long)S.tf_off[0], (unsigned long long)S.tf_off[1], (unsigned long long)S.tf_off[2], (unsigned long long)S.tf_off[3]); state_op(h.op, a); break; }
            case OP_RAST_DISCARD: PD("Y rasterizer discard %d", ((const p_rast_discard *)a)->on); state_op(h.op, a); break;
            case OP_COPY_B2T: copy_b2t(a); break;
            case OP_COPY_T2B: copy_t2b(a); break;
            case OP_COPY_T2T: copy_t2t(a); break;
            case OP_CLEAR_TEX: clear_tex(a); break;
            case OP_DISPATCH: dispatch(a); break;
            case OP_BARRIER: {
                int32_t bits; memcpy(&bits, a, 4);
                if (bits) {
                    g_game_barrier = 1;
                    rp_end();
                    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
                    args_dirty_all();
                    if (!g_explicit_barriers++) MLOG("NVN Barrier %#x: ends active pass and orders later access through the alias fence", bits);
                }
                break;
            }
            case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: break;
            default: break;
        }
        p += h.size;
    }
}

// ---------- Present: draw the swapchain texture to the CAMetalLayer; optional screenshot ----------
static id<MTLRenderPipelineState> g_blit_pso; static MTLPixelFormat g_blit_fmt;
static id<MTLSamplerState> g_blit_samp;
static uint64_t g_present_n;
static _Atomic uint64_t g_displayed_n;
uint64_t mtl_present_count(void) { return atomic_load(&g_displayed_n); }   // Actual drawable presentations, not attempts to queue a frame.
void mtl_res_snapshot(mtl_res_count *out) {
    memset(out, 0, sizeof *out);
    out->pso = g_pso.n;
    out->cps = g_cps.n;
    out->dss = g_dss.n;
    out->shaders = (uint32_t)(g_shcache ? g_shcache.count : 0);
    os_unfair_lock_lock(&g_c1_lock);
    out->c1_bufs = (uint32_t)(g_c1_banks.count + g_c1_exclusive); out->c1_alloc = g_c1_alloc_bytes; out->c1_data = g_c1_data_bytes;
    os_unfair_lock_unlock(&g_c1_lock);
    pthread_mutex_lock(&g_smu);
    out->live_tex = g_by_serial.n;
    pthread_mutex_unlock(&g_smu);
    pthread_mutex_lock(&g_mu);
    out->pools = (uint32_t)g_npool;
    out->pool_adds = g_pool_adds;
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&g_rmu);
    out->view_slots = (uint32_t)g_ntreg;
    for (int i = 0; i < g_ntreg; i++) if (g_treg[i].view) out->view_nonnull++;
    for (int i = 0; i < g_nsreg; i++) if (g_sview[i]) out->sview_nonnull++;
    pthread_mutex_unlock(&g_rmu);
}
// Resource counts (read by long-run stability L3): the host calls it per game tick / event (every 250 ticks, loading screen dismissed, exit), not sampled by wall clock / present count.
// Line format: [nvn] 资源计数 拍 <拍号> <触发>：<名> <数值> … ([nvn] resource counts tick <tick number> <trigger>: <name> <value> ...) (names contain no spaces, single-space separated); new fields are appended to the end of this line
void mtl_res_counts(uint32_t tick, const char *why) {
    mtl_res_count c;
    mtl_res_snapshot(&c);
    int dl, du; uint64_t da; nvn_dirty_slot_stats(&dl, &du, &da);
    LOG("[nvn] 资源计数 拍 %u %s：渲染管线 %u 计算管线 %u 着色器 %u 深度模板 %u view非空 %u view槽 %u 存活贴图 %u 内存池存活 %u 内存池累计 %llu 采样器view非空 %u 脏页区间存活 %d 脏页区间累计 %llu c1缓冲 %u c1字节 %llu c1实际 %llu",
        tick, why, c.pso, c.cps, c.shaders, c.dss, c.view_nonnull, c.view_slots, c.live_tex,
        c.pools, (unsigned long long)c.pool_adds, c.sview_nonnull, dl, (unsigned long long)da, c.c1_bufs, (unsigned long long)c.c1_alloc, (unsigned long long)c.c1_data);
}
// Screenshot on request (used by DM_NVN_SHOT_UI): dm_gfx_shot_at(k, path) = take a screenshot at the kth frame after the current present frame; due requests are checked at every frame's present
#define SHOTQ_MAX 8
static struct { uint64_t frame; char path[1024]; } g_shotq[SHOTQ_MAX]; static int g_nshotq;
static pthread_mutex_t g_shotq_mu = PTHREAD_MUTEX_INITIALIZER;
static int present_encode(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4]);
static int present_encode_m4(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4], int uv_i);
// Game picture needed by the whole-window screenshot (front end window_shot; no longer does drawViewHierarchy on a Metal layer with the display link attached): at the next present, the present thread draws the same frame
// with the same encoding as the present (present_encode) into an offscreen texture of the same size as the swapchain, and after the GPU finishes hands out a CGImage in the completion callback (the callback does not block the present). Guarded by g_shotq_mu
static void (^g_grab_cb)(CGImageRef img);
void mtl_grab_next_present(void (^cb)(CGImageRef img)) { pthread_mutex_lock(&g_shotq_mu); g_grab_cb = [cb copy]; pthread_mutex_unlock(&g_shotq_mu); }
static void grab_present(id<MTLTexture> src, const int crop[4], CGSize sz) {
    pthread_mutex_lock(&g_shotq_mu); void (^cb)(CGImageRef) = g_grab_cb; g_grab_cb = nil; pthread_mutex_unlock(&g_shotq_mu);
    if (!cb) return;
    size_t w = (size_t)sz.width, h = (size_t)sz.height, row = w * 4;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> out = w && h ? [g_dev newTextureWithDescriptor:d] : nil; id<MTLBuffer> buf = out ? [g_dev newBufferWithLength:row * h options:MTLResourceStorageModeShared] : nil;
    id<MTLCommandBuffer> cb2 = buf ? q_cb() : nil;
    if (!cb2 || !present_encode(cb2, src, out, crop)) { MLOG("【红】整窗截图：游戏画面离屏编码失败（%zux%zu）", w, h); cb(NULL); return; }
    id<MTLBlitCommandEncoder> b = [cb2 blitCommandEncoder];
    [b copyFromTexture:out sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [b endEncoding];
    [cb2 addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c;
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(buf.contents, w, h, 8, row, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
        CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL; cb(img);
        CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs); }];
    [cb2 commit];
}
void dm_gfx_shot_at(long k, const char *path) {
    pthread_mutex_lock(&g_shotq_mu);
    if (g_nshotq < SHOTQ_MAX) { g_shotq[g_nshotq].frame = g_present_n + (uint64_t)(k > 0 ? k : 0); strlcpy(g_shotq[g_nshotq].path, path, sizeof g_shotq[0].path); g_nshotq++; }
    else MLOG("【红】截图请求超过 %d 个，丢弃 %s", SHOTQ_MAX, path);
    pthread_mutex_unlock(&g_shotq_mu);
}
// DM_NVN_SHOT_OUT=WIDTHxHEIGHT (for picture quality metrics): the present-frame screenshot instead captures the result "upscaled to this resolution by present_encode", i.e. the picture the player sees on screen; screen=0 (texture dump) is unaffected
static void shot(id<MTLTexture> src, const char *path, int screen, const int crop[4]) {
    static int out_w = -1, out_h;
    if (out_w < 0) {
        out_w = 0;
        const char *e = getenv("DM_NVN_SHOT_OUT"); int w = 0, h = 0; char sep = 0, extra = 0;
        if (e && sscanf(e, "%d%c%d%c", &w, &sep, &h, &extra) == 3 && sep == 'x' && w > 0 && h > 0) { out_w = w; out_h = h; }
    }
    id<MTLTexture> read = src;
    if (out_w && screen) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:out_w height:out_h mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> out = [g_dev newTextureWithDescriptor:d];
        if (!out) { MLOG("【红】截图输出贴图建不出 %dx%d", out_w, out_h); return; }
        id<MTLCommandBuffer> cb = q_cb();
        if (!present_encode(cb, src, out, crop)) return;
        [cb commit]; [cb waitUntilCompleted];
        read = out;
    }
    if (read.pixelFormat != MTLPixelFormatRGBA8Unorm && read.pixelFormat != MTLPixelFormatRGBA8Unorm_sRGB &&
        read.pixelFormat != MTLPixelFormatBGRA8Unorm && read.pixelFormat != MTLPixelFormatBGRA8Unorm_sRGB) { MLOG("【红】截图不支持格式 %lu", (unsigned long)read.pixelFormat); return; }
    size_t w = read.width, h = read.height, row = w * 4;
    id<MTLBuffer> buf = [g_dev newBufferWithLength:row * h options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb();
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:read sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (screen && read == src && atomic_load_explicit(&g_origin_ll, memory_order_relaxed)) {   // lower-left origin: the window image is stored bottom-up (see g_origin_ll)
        uint8_t *p = buf.contents, *tmp = malloc(row);
        if (tmp) { for (size_t y = 0; y < h / 2; y++) { memcpy(tmp, p + y * row, row); memcpy(p + y * row, p + (h - 1 - y) * row, row); memcpy(p + (h - 1 - y) * row, tmp, row); } free(tmp); } }
    int bgra = read.pixelFormat == MTLPixelFormatBGRA8Unorm || read.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGBitmapInfo bi = bgra ? (kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little) : (CGBitmapInfo)kCGImageAlphaNoneSkipLast;
    CGContextRef ctx = CGBitmapContextCreate(buf.contents, w, h, 8, row, cs, bi);
    CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dst = url ? CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL) : NULL;
    // If any step yields null (directory does not exist / not writable, bitmap context cannot be created), report a failure and return; previously it went straight to CFRelease(NULL) and crashed in CFRelease.cold (hit during a 1952-wide TEXDUMP)
    int ok = 0;
    if (!ctx || !img || !dst) MLOG("【红】截图 %s 失败：%s", path, !ctx ? "位图上下文建不出" : !img ? "取图失败" : "写不出（目录不存在或不可写）");
    else { CGImageDestinationAddImage(dst, img, NULL); ok = CGImageDestinationFinalize(dst); }
    if (dst) CFRelease(dst); if (url) CFRelease(url); CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs);
    dm_shot_written(path, ok);   // Let the host write the 「冻结拍」(frozen tick) line first, then write the 「成功」(success) line: the screenshot script stops the host as soon as it sees 「成功」(success), and with the order reversed the log would lack the 「冻结拍」(frozen tick) line under full load (false failure in the gate check's require_log)
    MLOG("截图 %zux%zu 格式 %lu → %s %s", w, h, (unsigned long)read.pixelFormat, path, ok ? "成功" : "失败");
    if (g_ruler_truth && getenv("DM_NVN_RULER") && !strcmp(getenv("DM_NVN_RULER"), "1") && ok) { static int shots; if (++shots == 2) g_ruler_pending = 1; }
    if (g_defer_ab >= 0 && getenv("DM_NVN_DEFER_AB") && !strcmp(getenv("DM_NVN_DEFER_AB"), "1") && ok) { static int shots; if (++shots == 2) { g_defer_ab_on = 1; MLOG("DM_NVN_DEFER_AB：第二张截图写盘完成，打开推迟合并通道"); } }
    if (g_udc_ab >= 0 && getenv("DM_NVN_UDC_AB") && !strcmp(getenv("DM_NVN_UDC_AB"), "1") && ok) {
        static int shots;
        if (++shots == 2) { g_udc_ab_on = 1; MLOG("DM_NVN_UDC_AB：第二张截图写盘完成，打开 DM_NVN_UNWRITTEN_DC"); }
    }
}
// ---------- DM_NVN_TEXDUMP=frame number:path (%d in the path is replaced with "index*100+mip level"): save each color render target used by that frame as an image ----------
// Diagnostic: before the first render pass of the next frame is opened, wait for the GPU to finish, then draw each one (the mip level used by that frame) by full-screen sampling into RGBA8 and save as PNG (float formats are clamped to [0,1])
static long g_td_frame = -2; static const char *g_td_path; static NSMutableArray *g_td_list; static int g_td_done;
static void td_dump(void);
// DM_NVN_TEXDUMP=size=WxH:path instead picks the frame by content: the first frame that opens a render pass whose first color attachment level is WxH (for one-off work whose frame differs between runs)
static unsigned g_td_w, g_td_h;
void td_note(MTLRenderPassDescriptor *pd, NSUInteger nc) {
    if (g_td_frame == -2) { const char *e = getenv("DM_NVN_TEXDUMP"), *c = e ? strchr(e, ':') : NULL; g_td_frame = c ? atol(e) : -1; g_td_path = c ? c + 1 : NULL; g_td_list = [NSMutableArray array];
        if (c && !strncmp(e, "size=", 5) && sscanf(e + 5, "%ux%u", &g_td_w, &g_td_h) == 2) g_td_frame = LONG_MAX; }
    if (g_td_frame < 0 || g_td_done) return;
    long f = (long)atomic_load(&g_cur_frame);
    if (g_td_frame == LONG_MAX) { id<MTLTexture> t0 = nc ? pd.colorAttachments[0].texture : nil; NSUInteger lv = pd.colorAttachments[0].level;
        if (t0 && MAX(t0.width >> lv, 1ul) == g_td_w && MAX(t0.height >> lv, 1ul) == g_td_h) { g_td_frame = f; MLOG("[texdump] 首个 %ux%u 渲染目标在帧 %ld：转储这一帧", g_td_w, g_td_h, f); } }
    if (f > g_td_frame) { td_dump(); return; }
    if (f != g_td_frame) return;
    for (NSUInteger i = 0; i < nc && i < 8; i++) {
        id<MTLTexture> t = pd.colorAttachments[i].texture; if (!t) continue;
        uint64_t ser = i < (NSUInteger)R.rt.n && R.rt.color[i] ? R.rt.color[i]->serial : 0;
        NSUInteger lv = pd.colorAttachments[i].level, sl = t.textureType == MTLTextureType2DArray ? pd.colorAttachments[i].slice : 0;
        BOOL seen = NO; for (NSArray *e in g_td_list) if ([e[1] unsignedLongLongValue] == ser && [e[2] unsignedIntegerValue] == lv && [e[3] unsignedIntegerValue] == sl) seen = YES;
        if (!seen) [g_td_list addObject:@[t, @(ser), @(lv), @(sl)]];
    }
}
static void td_dump(void) {
    g_td_done = 1;
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    mtl_finish_gpu();
    static id<MTLRenderPipelineState> ps; static id<MTLSamplerState> sp;
    if (!ps) {
        static const char *src_msl =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; float2 uv; };\n"
            "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
            "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { float4 c = t.sample(s, in.uv); return float4(c.rgb, 1); }\n";
        NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src_msl) options:nil error:&e];
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"vs"]; d.fragmentFunction = [lib newFunctionWithName:@"fs"];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        ps = nvn_cached_render(g_dev, d, &e);
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sp = [g_dev newSamplerStateWithDescriptor:sd];
        if (!ps) { MLOG("【红】TEXDUMP 管线失败 %s", e.localizedDescription.UTF8String); return; }
    }
    MLOG("[texdump] 帧 %ld 共 %lu 张颜色目标", g_td_frame, (unsigned long)g_td_list.count);
    // DM_NVN_TEXRAW=index,...: save raw bytes by texture index (including 3D; file: %d in the path is replaced with the index, suffix .raw, first-line text header: width height depth format bytes-per-row)
    for (const char *q = getenv("DM_NVN_TEXRAW"); q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) {
        if (!strncmp(q, "info", 4)) {   // DM_NVN_TEXRAW=info: the textures remembered by DM_NVN_TEXINFO
            static char buf[256]; size_t o = 0; for (int i = 0; i < g_texinfo_n; i++) o += snprintf(buf + o, sizeof buf - o, "%s%llu", i ? "," : "", (unsigned long long)g_texinfo_ser[i]);
            if (o) { static char env[300]; snprintf(env, sizeof env, "%s", buf); setenv("DM_NVN_TEXRAW", env, 1); q = getenv("DM_NVN_TEXRAW"); MLOG("[texraw] info → %s", q); } else continue;
        }
        uint64_t ser = (uint64_t)atoll(q); id o = tex_get(ser);
        id<MTLTexture> t = [o isKindOfClass:[NvPkg class]] ? ((NvPkg *)o)->tex : o;
        if (!t) { MLOG("[texraw] ser=%llu 不在序号表", ser); continue; }
        int bw, bh; NSUInteger w = t.width, h = t.height, dz = t.depth;
        size_t bpp = t.pixelFormat == MTLPixelFormatRGBA16Float ? 8 : t.pixelFormat == MTLPixelFormatRGBA32Float ? 16 : 4; (void)bw; (void)bh;
        size_t row = w * bpp, img = row * h;
        id<MTLBuffer> buf = [g_dev newBufferWithLength:img * dz options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, dz) toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:img];
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        char path[1024]; snprintf(path, sizeof path, g_td_path, (int)ser); strlcat(path, ".raw", sizeof path);
        FILE *f = fopen(path, "wb"); if (f) { fprintf(f, "%lu %lu %lu %lu %zu\n", (unsigned long)w, (unsigned long)h, (unsigned long)dz, (unsigned long)t.pixelFormat, row); fwrite(buf.contents, 1, img * dz, f); fclose(f); }
        MLOG("[texraw] ser=%llu %lux%lux%lu 格式 %lu → %s", ser, (unsigned long)w, (unsigned long)h, (unsigned long)dz, (unsigned long)t.pixelFormat, path);
    }
    for (NSArray *e in g_td_list) {
        id<MTLTexture> t = e[0]; uint64_t ser = [e[1] unsignedLongLongValue]; NSUInteger lv = [e[2] unsignedIntegerValue], sl = [e[3] unsignedIntegerValue];
        // 2D arrays are saved by the attached layer (file index ser*100 + layer*10 + level; layer 0 is named like 2D)
        if (t.textureType != MTLTextureType2D && t.textureType != MTLTextureType2DArray) { MLOG("[texdump] ser=%llu 类型 %lu 跳过", ser, (unsigned long)t.textureType); continue; }
        if (getenv("DM_NVN_SNAPRAW")) {   // Also save the raw bytes (converting an HDR target to an 8-bit image truncates; first-line text header: width height format bytes-per-row)
            NSUInteger rw = MAX(t.width >> lv, 1u), rh = MAX(t.height >> lv, 1u);
            size_t bpp = t.pixelFormat == MTLPixelFormatRGBA16Float ? 8 : t.pixelFormat == MTLPixelFormatRGBA32Float ? 16 : t.pixelFormat == MTLPixelFormatR16Float || t.pixelFormat == MTLPixelFormatR16Unorm ? 2 : 4;
            id<MTLBuffer> rb = [g_dev newBufferWithLength:rw * rh * bpp options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> rc = q_cb(); id<MTLBlitCommandEncoder> bb = [rc blitCommandEncoder];
            [bb copyFromTexture:t sourceSlice:0 sourceLevel:lv sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(rw, rh, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:rw * bpp destinationBytesPerImage:rw * rh * bpp];
            [bb endEncoding]; [rc commit]; [rc waitUntilCompleted];
            char rp_[1024]; snprintf(rp_, sizeof rp_, g_td_path, (int)(ser * 100 + lv)); strlcat(rp_, ".raw", sizeof rp_);
            FILE *f = fopen(rp_, "wb"); if (f) { fprintf(f, "%lu %lu %lu %zu\n", (unsigned long)rw, (unsigned long)rh, (unsigned long)t.pixelFormat, rw * bpp); fwrite(rb.contents, 1, rw * rh * bpp, f); fclose(f); }
        }
        id<MTLTexture> v = [t newTextureViewWithPixelFormat:t.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(lv, 1) slices:NSMakeRange(sl, 1)];
        NSUInteger w = MAX(t.width >> lv, 1u), h = MAX(t.height >> lv, 1u);
        MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        od.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; od.storageMode = MTLStorageModePrivate;
        id<MTLTexture> o = [g_dev newTextureWithDescriptor:od];
        id<MTLCommandBuffer> cb = q_cb();
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = o; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
        [en setRenderPipelineState:ps]; [en setFragmentTexture:v atIndex:0]; [en setFragmentSamplerState:sp atIndex:0];
        [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
        char path[1024]; snprintf(path, sizeof path, g_td_path, (int)(ser * 100 + sl * 10 + lv));
        MLOG("[texdump] ser=%llu lv=%lu 层 %lu 格式 %lu %lux%lu", ser, (unsigned long)lv, (unsigned long)sl, (unsigned long)t.pixelFormat, (unsigned long)w, (unsigned long)h);
        shot(o, path, 0, NULL);
    }
    [g_td_list removeAllObjects];
}
// DM_NVN_TEXSNAP=in-frame index,... (together with the frame number and path of DM_NVN_TEXDUMP): as soon as these render passes end, copy their color attachments and save them together with the end-of-frame dump
// (the same target is often cleared and reused within a frame, so the end-of-frame state cannot reflect the output of intermediate passes); file name index = in-frame index*100000 + texture index*100 + mip level
static int g_ts_idx[64], g_ts_n = -1, g_ts_on; static uint64_t g_ts_first, g_ts_curf = ~0ull;
static __strong id<MTLTexture> g_ts_att[8]; static uint64_t g_ts_ser[8]; static NSUInteger g_ts_lv[8]; static int g_ts_na;
void ts_note(MTLRenderPassDescriptor *pd, NSUInteger nc) {
    if (g_ts_n < 0) { g_ts_n = 0; const char *e = getenv("DM_NVN_TEXSNAP");
        for (const char *q = e; q && *q && g_ts_n < 64; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) g_ts_idx[g_ts_n++] = atoi(q); }
    g_ts_on = 0; if (!g_ts_n || g_td_frame < 0 || g_td_done) return;
    uint64_t f = atomic_load(&g_cur_frame); if (f != g_ts_curf) { g_ts_curf = f; g_ts_first = g_stat_pass; }
    if ((long)f != g_td_frame) return;
    int idx = (int)(g_stat_pass - g_ts_first); for (int i = 0; i < g_ts_n; i++) if (g_ts_idx[i] == idx) g_ts_on = idx + 1;
    g_ts_na = 0; if (!g_ts_on) return;
    for (NSUInteger i = 0; i < nc && i < 8; i++) { id<MTLTexture> t = pd.colorAttachments[i].texture; if (!t) continue;
        g_ts_att[g_ts_na] = t; g_ts_ser[g_ts_na] = R.rt.color[i] ? R.rt.color[i]->serial : 0; g_ts_lv[g_ts_na] = pd.colorAttachments[i].level; g_ts_na++; }
}
void ts_after(void) {
    if (!g_ts_on) return;
    id<MTLCommandBuffer> dcb = q_cb();
    for (int i = 0; i < g_ts_na; i++) {
        id<MTLTexture> t = g_ts_att[i]; if (t.textureType != MTLTextureType2D && t.textureType != MTLTextureType2DArray) continue;   // 2D arrays take slice 0
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:t.width height:t.height mipmapped:t.mipmapLevelCount > 1];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate; d.mipmapLevelCount = t.mipmapLevelCount;
        id<MTLTexture> c = [g_dev newTextureWithDescriptor:d];
        id<MTLBlitCommandEncoder> b = [dcb blitCommandEncoder];
        [b copyFromTexture:t sourceSlice:0 sourceLevel:g_ts_lv[i] toTexture:c destinationSlice:0 destinationLevel:g_ts_lv[i] sliceCount:1 levelCount:1]; [b endEncoding];
        [g_td_list addObject:@[c, @((uint64_t)(g_ts_on - 1) * 1000 + g_ts_ser[i]), @(g_ts_lv[i])]];
    }
    g_ts_on = 0;
    [dcb commit];
}
// Ring of actual presentation times: present sequence number -> presentedTime (seconds; -1 = dropped by the system, not presented)
#define PRING 1024
typedef struct { uint64_t n; double t; } pres_ent;
static pres_ent g_pres[PRING];
// Sparse software enqueue-to-display probes. A single ticket joins callbacks;
// this is not physical input latency or the whole frame's GPU critical path.
@interface NvPresentProbe : NSObject {
@public os_unfair_lock lock; uint64_t ticket, frame; unsigned ready;
    double queued, commit, scheduled, gpu_start, gpu_end, shown;
}
@end
@implementation NvPresentProbe @end
static void present_probe_note(NvPresentProbe *p, unsigned phase, double a, double b) {
    if (!p) return;
    os_unfair_lock_lock(&p->lock);
    if (phase == 1) p->scheduled = a;
    else if (phase == 2) { p->gpu_start = a; p->gpu_end = b; }
    else p->shown = a;
    p->ready |= phase;
    if (p->ready == 7) {
        p->ready |= 8;
        MLOG("[呈现采样] ticket=%llu frame=%llu enqueue=%.6f commit=%.6f scheduled=%.6f gpu_start=%.6f gpu_end=%.6f displayed=%.6f queue_to_display_ms=%.3f",
            (unsigned long long)p->ticket,(unsigned long long)p->frame,p->queued,p->commit,p->scheduled,
            p->gpu_start,p->gpu_end,p->shown,p->shown > 0 ? (p->shown-p->queued)*1000 : -1);
    }
    os_unfair_lock_unlock(&p->lock);
}
// Get the actual presentation time of the nth present (returns 0 if not ready, -1 if dropped)
double mtl_presented_time(uint64_t n) { pres_ent e = g_pres[n % PRING]; return e.n == n ? e.t : 0; }
static _Atomic uint64_t g_drawable_ns;   // Accumulated wall clock waiting for nextDrawable in present (timing instrumentation)
uint64_t mtl_drawable_ns_take(void) { return atomic_exchange(&g_drawable_ns, 0); }
// Maximum refresh rate (Hz) of the screen when there is a window; returns 0 without a window. Mac uses CoreGraphics display modes (thread-safe C interface; the present configuration is read on the game thread)
int mtl_display_hz(void) {
    if (!front_metal_layer()) return 0;
#if TARGET_OS_OSX
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
    double hz = m ? CGDisplayModeGetRefreshRate(m) : 0; if (m) CGDisplayModeRelease(m);
    return (int)(hz + 0.5);
#else
    Class c = NSClassFromString(@"UIScreen"); id s = c ? [c valueForKey:@"mainScreen"] : nil;
    return s ? [[s valueForKey:@"maximumFramesPerSecond"] intValue] : 0;
#endif
}
static _Atomic uint64_t g_present_stall_ns;   // For unit tests: an extra pause before each present on the present thread (simulating a drawable that stays unavailable)

void mtl_present_test_stall(uint64_t ns) { g_present_stall_ns = ns; }
static void present_pipeline(MTLPixelFormat format) {
    if (!g_blit_pso || g_blit_fmt != format) {
        static const char *src_msl =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; float2 uv; };\n"
            "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
            "struct Crop { float4 rect; float4 edge; };\n"
            "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) { return t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw)); }\n";
        NSError *e = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src_msl) options:nil error:&e];
        if (!lib) MDIE("出图着色器编译失败 %s", e.localizedDescription.UTF8String);
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = format;
        g_blit_pso = nvn_cached_render(g_dev, pd, &e);
        if (!g_blit_pso) MDIE("出图管线失败 %s", e.localizedDescription.UTF8String);
        g_blit_fmt = format;
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear; sd.supportArgumentBuffers = YES;
        g_blit_samp = [g_dev newSamplerStateWithDescriptor:sd];
    }
}
// Present samples only the valid source rectangle declared by NVN; kept separate from the raw swapchain readback screenshot. Edges are clamped to the texel centers of the valid area, so bilinear does not read the black texels outside.
// Present samples the raw bytes stored in the swapchain texture (the Switch's scan-out sends the stored bytes as they are): an sRGB source is replaced by a non-sRGB view of the same storage,
// otherwise sampling decodes to linear first, the BGRA8Unorm layer does not encode again, and the whole picture comes out dark. Other formats are returned as they are (Mario's swapchain is RGBA8 and does not come here).
// View cache of 4 entries: swapchain textures live long and are few; a view holds its parent, so an equal parent pointer means the same object. A replaced view is released after the submissions that used it complete (Metal 4 does not retain resources)
static id<MTLTexture> present_raw_src(id<MTLTexture> src) {
    MTLPixelFormat pf = src.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB ? MTLPixelFormatRGBA8Unorm : src.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatInvalid;
    if (pf == MTLPixelFormatInvalid) return src;
    static __strong id<MTLTexture> views[4]; static int next; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    id<MTLTexture> v = nil;
    for (int i = 0; i < 4 && !v; i++) if (views[i] && views[i].parentTexture == src) v = views[i];
    if (!v && (v = [src newTextureViewWithPixelFormat:pf])) { if (views[next]) defer_obj_unref(views[next]); views[next] = v; next = (next + 1) & 3; }
    pthread_mutex_unlock(&mu);
    if (!v) { RED_ONCE(69, "出图：sRGB 交换链贴图的非 sRGB 视图建不出（格式 %lu），按解码后的值出图", (unsigned long)src.pixelFormat); return src; }
    return v;
}
static int present_encode(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4]) {
    NSUInteger cx = 0, cy = 0, cw = src.width, ch = src.height;
    if (crop && (crop[2] || crop[3])) {
        if (crop[0] < 0 || crop[1] < 0 || crop[2] <= 0 || crop[3] <= 0 ||
            (uint64_t)crop[0] + crop[2] > src.width || (uint64_t)crop[1] + crop[3] > src.height) {
            MLOG("【红】出图裁切越界：%d,%d %dx%d / 原纹理 %lux%lu", crop[0], crop[1], crop[2], crop[3], (unsigned long)src.width, (unsigned long)src.height); return 0;
        }
        cx = crop[0]; cy = crop[1]; cw = crop[2]; ch = crop[3];
    }
    static uint64_t last_crop[8]; uint64_t meta[8] = {src.width, src.height, cx, cy, cw, ch, dst.width, dst.height};
    if (memcmp(meta, last_crop, sizeof meta)) { memcpy(last_crop, meta, sizeof meta);
        MLOG("出图裁切：原纹理 %lux%lu，有效 %lu,%lu %lux%lu → 目标 %lux%lu", (unsigned long)src.width, (unsigned long)src.height,
             (unsigned long)cx, (unsigned long)cy, (unsigned long)cw, (unsigned long)ch, (unsigned long)dst.width, (unsigned long)dst.height); }
    present_pipeline(dst.pixelFormat);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    // Keep the game picture's aspect ratio and center it, leaving black bars in the rest (no stretching when the screen ratio differs from the game's, e.g. iPhone about 19.5:9 versus the game's 16:9);
    // when the ratio difference is < 1% (the remainder from the game width being a multiple of 8 in the full-screen setup, e.g. 1952x900 versus 2868x1320 differs by 0.17%) fill directly, leaving no few-pixel border
    double dw = dst.width, dh = dst.height, s = fmin(dw / cw, dh / ch);
    MTLViewport vp = {floor((dw - cw * s) / 2), floor((dh - ch * s) / 2), round(cw * s), round(ch * s), 0, 1};
    if (fabs((dw / dh) / ((double)cw / ch) - 1) < 0.01) vp = (MTLViewport){0, 0, dw, dh, 0, 1};
    int full = vp.originX == 0 && vp.originY == 0 && vp.width == dw && vp.height == dh;
    static double last_vp[4];   // Log one line when the viewport changes (used by the real-device screen-level acceptance screen_check.py to check the aspect ratio)
    if (memcmp(last_vp, &vp, sizeof last_vp)) { memcpy(last_vp, &vp, sizeof last_vp);
        MLOG("出图视口 %.0f,%.0f %.0fx%.0f / 画布 %.0fx%.0f（游戏画面 %lux%lu）", vp.originX, vp.originY, vp.width, vp.height, dw, dh, (unsigned long)src.width, (unsigned long)src.height); }
    rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = full ? MTLLoadActionDontCare : MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setViewport:vp];
    [en setRenderPipelineState:g_blit_pso]; [en setFragmentTexture:present_raw_src(src) atIndex:0]; [en setFragmentSamplerState:g_blit_samp atIndex:0];
    float uv[8] = {(float)cx / src.width, (float)cy / src.height, (float)cw / src.width, (float)ch / src.height,
                   (cx + 0.5f) / src.width, (cy + 0.5f) / src.height, (cx + cw - 0.5f) / src.width, (cy + ch - 0.5f) / src.height};
    if (atomic_load_explicit(&g_origin_ll, memory_order_relaxed)) { uv[1] += uv[3]; uv[3] = -uv[3]; }   // lower-left origin: the window image is stored bottom-up
    [en setFragmentBytes:uv length:sizeof uv atIndex:0];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [en endEncoding];
    return 1;
}
static int present_encode_m4(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4], int uv_i) {
    NSUInteger cx = 0, cy = 0, cw = src.width, ch = src.height;
    if (crop && (crop[2] || crop[3])) {
        if (crop[0] < 0 || crop[1] < 0 || crop[2] <= 0 || crop[3] <= 0 ||
            (uint64_t)crop[0] + crop[2] > src.width || (uint64_t)crop[1] + crop[3] > src.height) {
            MLOG("【红】出图裁切越界：%d,%d %dx%d / 原纹理 %lux%lu", crop[0], crop[1], crop[2], crop[3], (unsigned long)src.width, (unsigned long)src.height); return 0;
        }
        cx = crop[0]; cy = crop[1]; cw = crop[2]; ch = crop[3];
    }
    present_pipeline(dst.pixelFormat);
    double dw = dst.width, dh = dst.height, s = fmin(dw / cw, dh / ch);
    MTLViewport vp = {floor((dw - cw * s) / 2), floor((dh - ch * s) / 2), round(cw * s), round(ch * s), 0, 1};
    if (fabs((dw / dh) / ((double)cw / ch) - 1) < 0.01) vp = (MTLViewport){0, 0, dw, dh, 0, 1};
    // This is the active iOS17 path. The other present encoder's crop/viewport
    // diagnostics did not describe these frames; record changes here as well.
    static uint64_t last_layout[8];
    uint64_t layout[8] = {src.width, src.height, cx, cy, cw, ch, dst.width, dst.height};
    if (memcmp(layout, last_layout, sizeof layout)) {
        memcpy(last_layout, layout, sizeof layout);
        MLOG("[呈现几何] 源 %lux%lu 有效 %lu,%lu %lux%lu；目标 %lux%lu；视口 %.0f,%.0f %.0fx%.0f；横纵缩放比 %.6f",
             (unsigned long)src.width, (unsigned long)src.height, (unsigned long)cx, (unsigned long)cy,
             (unsigned long)cw, (unsigned long)ch, (unsigned long)dst.width, (unsigned long)dst.height,
             vp.originX, vp.originY, vp.width, vp.height, (vp.width / cw) / (vp.height / ch));
    }
    int full = vp.originX == 0 && vp.originY == 0 && vp.width == dw && vp.height == dh;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = full ? MTLLoadActionDontCare : MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    if (!en) { MLOG("【红】iOS17 出图渲染编码器创建失败"); return 0; }


    [en setFragmentTexture:present_raw_src(src) atIndex:0];
    [en setFragmentSamplerState:g_blit_samp atIndex:0];
    float *uv = g_present_uv[uv_i].contents;
    uv[0] = (float)cx / src.width; uv[1] = (float)cy / src.height; uv[2] = (float)cw / src.width; uv[3] = (float)ch / src.height;
    uv[4] = (cx + 0.5f) / src.width; uv[5] = (cy + 0.5f) / src.height; uv[6] = (cx + cw - 0.5f) / src.width; uv[7] = (cy + ch - 0.5f) / src.height;
    if (atomic_load_explicit(&g_origin_ll, memory_order_relaxed)) { uv[1] += uv[3]; uv[3] = -uv[3]; }   // lower-left origin: the window image is stored bottom-up
    [en setFragmentBuffer:g_present_uv[uv_i] offset:0 atIndex:0];
    [en setViewport:vp]; [en setRenderPipelineState:g_blit_pso];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    m4_end(en);
    return 1;
}
static double g_link_it_tin;   // Enqueue time of the frame the display-link callback is presenting (read and written only on the display-link thread)
static void classic_present_done(void) {
    pthread_mutex_lock(&g_pq_mu);
    g_classic_pending--;
    pthread_cond_broadcast(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
}
static int present_now(const tex_t *t, double period, uint64_t frame, uint64_t ticket, const int crop[4], id<CAMetalDrawable> supplied, double target_ts, double queued_at) {
    if (g_present_stall_ns) { struct timespec ts = {0, (long)g_present_stall_ns}; nanosleep(&ts, NULL); }
    g_present_n++;
    if (g_use != USE_OFF && g_use != USE_DONE) use_tick();
    if (bootprof() && (g_present_n == 1 || g_present_n % 120 == 0)) {
        if (g_present_n == 1) g_bp_first_present = bp_now() - g_bp_t0;
        MLOG("[bootprof] t=%.3f s 出图第 %llu 次：metallib 载入 %llu 个 / %.1f ms；管线创建 %llu 个 / %.1f ms（均在提交线程同步）；预热领用 着色器 %llu / 渲染管线 %llu / 计算管线 %llu；"
             "贴图上传 %.1f MB / %llu 张首传 + %llu 次重传 / %.1f ms；首帧出图 t=%.3f s", (bp_now() - g_bp_t0) / 1e9, g_present_n,
             g_bp_lib_n, g_bp_lib_ns / 1e6, g_bp_pso_n, g_bp_pso_ns / 1e6, g_warm_claim_shaders, g_warm_claim_rps, g_warm_claim_cps,
             g_bp_up_bytes / 1048576.0, g_pkg_uploads, g_pkg_reuploads,
             g_up_total_ns / 1e6, g_bp_first_present / 1e9);
    }
    PD("X ser=%llu", t->serial);
    id<MTLTexture> src = tex_mtl(t);
    if (!src) { RED_ONCE(8, "出图贴图建不出 MTLTexture"); return 0; }
    // DM_NVN_REGLUM=x0,y0,x1,y1:file (normalized coordinates): read back this region on every present and record "present sequence number, present time ns, region mean luminance, subsampled grayscale thumbnail (hex)" (diagnostic: verify that animation speed is independent of frame rate).
    // A separate command buffer is queued after this frame's rendering (the same queue executes in submit order); does not wait for the GPU and does not disturb frame pacing
    static int rl = -1; static float rr[4]; static FILE *rlf; static pthread_mutex_t rlmu = PTHREAD_MUTEX_INITIALIZER;
    if (rl < 0) { const char *e = getenv("DM_NVN_REGLUM"), *c = e ? strchr(e, ':') : NULL;
        rl = c && sscanf(e, "%f,%f,%f,%f", &rr[0], &rr[1], &rr[2], &rr[3]) == 4 && (rlf = fopen(c + 1, "w")) != NULL;
        if (e && !rl) MLOG("【红】DM_NVN_REGLUM 参数或文件无效：%s", e); }
    if (rl && (src.pixelFormat == MTLPixelFormatRGBA8Unorm || src.pixelFormat == MTLPixelFormatBGRA8Unorm || src.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB || src.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB)) {
        NSUInteger x0 = (NSUInteger)(rr[0] * src.width), y0 = (NSUInteger)(rr[1] * src.height), w = (NSUInteger)(rr[2] * src.width) - x0, h = (NSUInteger)(rr[3] * src.height) - y0;
        id<MTLBuffer> rb = [g_dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x0, y0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
        [b endEncoding];
        unsigned long long n = g_present_n, tns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c; const uint8_t *q = rb.contents; uint64_t sum = 0;
            uint8_t g[4096]; NSUInteger ng = 0;   // Subsampled grayscale thumbnail (every 4th row / column, at most 4096 points), recorded along with the mean, for per-frame image correlation analysis
            for (NSUInteger y = 0; y < h; y += 4) for (NSUInteger x = 0; x < w && ng < sizeof g; x += 4) { const uint8_t *px = q + 4 * (y * w + x); g[ng++] = (uint8_t)((px[0] + px[1] + px[2]) / 3); }
            for (NSUInteger k = 0; k < w * h; k++) sum += q[4 * k] + q[4 * k + 1] + q[4 * k + 2];
            pthread_mutex_lock(&rlmu); fprintf(rlf, "%llu\t%llu\t%.4f\t", n, tns, sum / (3.0 * w * h));
            for (NSUInteger k = 0; k < ng; k++) fprintf(rlf, "%02x", g[k]);
            fputc('\n', rlf); fflush(rlf); pthread_mutex_unlock(&rlmu); }];
        [cb commit];
    }
    // DM_NVN_SHOT=frame number[,frame number...]:path (%d in the path is replaced with the frame number)
    static long shots[4096]; static int nshot = -1; static const char *shot_path;   // up to 4096 frames (a burst over a frame range, for scoring frame by frame)
    if (nshot < 0) {
        nshot = 0; const char *e = getenv("DM_NVN_SHOT"), *c = e ? strchr(e, ':') : NULL;
        if (c) { shot_path = c + 1; for (const char *q = e; q < c && nshot < 4096; q = strchr(q, ',') ? strchr(q, ',') + 1 : c) shots[nshot++] = atol(q); }
    }
    for (int i = 0; i < nshot; i++) if ((long)g_present_n == shots[i]) { char path[1024]; snprintf(path, sizeof path, shot_path, (int)g_present_n); mtl_finish_gpu(); shot(src, path, 1, crop); }
    pthread_mutex_lock(&g_shotq_mu);
    for (int i = 0; i < g_nshotq;) if (g_present_n >= g_shotq[i].frame) { mtl_finish_gpu(); shot(src, g_shotq[i].path, 1, crop); g_shotq[i] = g_shotq[--g_nshotq]; } else i++;
    pthread_mutex_unlock(&g_shotq_mu);

    CAMetalLayer *layer = (__bridge CAMetalLayer *)front_metal_layer();
    if (!layer) return 0;
#if TARGET_OS_OSX
    if (!layer.displaySyncEnabled) layer.displaySyncEnabled = YES;   // Windowed mode follows the display's vsync to prevent tearing
#endif
    present_pipeline(layer.pixelFormat);   // Same as the original path: the first frame compiles the pipeline before taking the drawable
    if (g_grab_cb) grab_present(src, crop, layer.drawableSize);   // Game picture needed by the whole-window screenshot (peek without the lock first; the real take locks inside the function)
    uint64_t dt0 = now_ns();
    id<CAMetalDrawable> dr = supplied ? supplied : [layer nextDrawable];
    if (!supplied) g_drawable_ns += now_ns() - dt0;
    if (!dr) return 0;
    int alloc_i = (int)(ticket % 3);
    if (g_present_alloc_fence[alloc_i]) {
        pthread_mutex_lock(&g_pq_mu);
        while (g_pq_done < g_present_alloc_fence[alloc_i]) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        pthread_mutex_unlock(&g_pq_mu);
    }
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    if (!present_encode_m4(cb, src, dr.texture, crop, alloc_i)) { return 0; }

    NvPresentProbe *probe = ticket % 120 == 0 ? [NvPresentProbe new] : nil;
    if (probe) { probe->lock = (os_unfair_lock)OS_UNFAIR_LOCK_INIT; probe->ticket=ticket;probe->frame=frame;probe->queued=queued_at; }
    [cb addCompletedHandler:^(id<MTLCommandBuffer> f) {
        if (f.error) MLOG("【红】上屏命令缓冲错误：%s", f.error.description.UTF8String);
#if TARGET_OS_SIMULATOR
        if (f.status == MTLCommandBufferStatusCompleted) atomic_fetch_add(&g_displayed_n, 1);
#endif
        present_probe_note(probe,2,f.GPUStartTime,f.GPUEndTime);
        pthread_mutex_lock(&g_pq_mu); if (ticket > g_pq_done) g_pq_done = ticket; g_pq_inflight--; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
    }];
    pthread_mutex_lock(&g_pq_mu); g_pq_inflight++; pthread_mutex_unlock(&g_pq_mu);
    g_present_alloc_fence[alloc_i] = ticket;
#if TARGET_OS_SIMULATOR   // The iOS simulator SDK has no presentation-time callback and no minimum display duration (used only for picture acceptance, display-link ticks are not counted)
    (void)period;
#else
    uint64_t pn = ticket;   // Record the present time by the game's present sequence number (ticket number): hle's present statistics are taken by "the game's nth present"; in the display link path the actual present count != the game's present count (old frames get merged and dropped)
    [dr addPresentedHandler:^(id<MTLDrawable> d) {   // Actual presentation time (the time this frame is really put on screen, CACurrentMediaTime base)
        CFTimeInterval tt = d.presentedTime;
        if (tt > 0) atomic_fetch_add(&g_displayed_n, 1);
        g_pres[pn % PRING] = (pres_ent){pn, tt > 0 ? tt : -1};
        present_probe_note(probe,4,tt > 0 ? tt : -1,0);
    }];
#endif
    // Scheduling stays asynchronous on both paths. A classic frame keeps its
    // layer handoff claim until the drawable's present call returns, not merely
    // until commit returns: attaching a link earlier forbids minimum duration.
    // GPU completion independently gates swapchain/UV-buffer reuse.
    [cb addScheduledHandler:^(id<MTLCommandBuffer> scheduled) {
        (void)scheduled;
        double scheduled_at = probe ? CACurrentMediaTime() : 0;
#if TARGET_OS_SIMULATOR
        [dr present];
#else
        if (!supplied && period > 0) [dr presentAfterMinimumDuration:period]; else [dr present];
#endif
        present_probe_note(probe,1,scheduled_at,0);
        if (!supplied) classic_present_done();
    }];
    if (probe) probe->commit = CACurrentMediaTime();
    [cb commit];
    if (target_ts > 0) {
        static double leads[300], e2e[300];   // e2e: game enqueue -> system-scheduled present (including the time waiting in the queue for the display-link callback), the full span of input latency that our side can read
        static uint64_t lead_n;
        leads[lead_n % 300] = (target_ts - CACurrentMediaTime()) * 1000.0; e2e[lead_n % 300] = g_link_it_tin > 0 ? (target_ts - g_link_it_tin) * 1000.0 : 0;
        lead_n++;
        if (lead_n % 300 == 0) {
            double a[300]; memcpy(a, leads, sizeof a);
            for (int i = 1; i < 300; i++) { double v = a[i]; int j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; }
            double b[300]; memcpy(b, e2e, sizeof b);
            for (int i = 1; i < 300; i++) { double v = b[i]; int j = i - 1; while (j >= 0 && b[j] > v) { b[j + 1] = b[j]; j--; } b[j + 1] = v; }
            MLOG("显示节拍 至 %llu：节拍 300、出图 %llu、空节拍 %llu（其中按出图间隔留拍 %llu）、合并丢弃旧帧 %llu；提交领先目标上屏 p50 %.3f ms / 最小 %.3f ms；游戏入队到目标上屏 p50 %.3f ms / 最小 %.3f / 最大 %.3f ms",
                 (unsigned long long)g_link_tick_n, (unsigned long long)g_present_n, (unsigned long long)(g_link_tick_n - g_present_n), (unsigned long long)g_link_held,
                 (unsigned long long)g_link_dropped, a[149], a[0], b[149], b[0], b[299]);
        }
    }
    (void)frame;
    return 1;
}
// ---------- Present thread ----------
// NVN semantics: QueuePresentTexture only queues the present and returns immediately (the game often calls it while holding a lock; iOS real device: synchronously waiting for nextDrawable inside it
// takes about 30 ms, the lock is held about 90% of the time, and the loading thread starves). Taking the drawable, blit and present are all done on this thread in enqueue order.
// Ticket number = which enqueue it is (starting from 1); g_pq_done = the ticket number that has been processed (blit committed to the GPU queue).
// Swapchain texture return: before AcquireTexture gets a texture it waits for the ticket of that texture's previous present to be processed (mtl_present_wait), after which the commands the game draws into it
// are necessarily ordered after that blit on the GPU queue.
#define PQ 8
typedef struct { tex_t t; double period; uint64_t frame; int crop[4]; double t_in; } pq_item;   // t_in: the time the game handed this frame into the present queue (CACurrentMediaTime)
static pq_item g_pq[PQ];
static uint64_t g_pq_in;
static int present_now(const tex_t *, double, uint64_t, uint64_t, const int[4], id<CAMetalDrawable>, double, double);

#if DM_HAVE_METAL_DISPLAY_LINK
@interface NvMetalDisplayLinkDelegate : NSObject <CAMetalDisplayLinkDelegate>
@end

// Fallback for the display link going silent while in the foreground (two smoke-test hangs before merge: the display link stopped calling back while the app was in the foreground, nobody takes the queued frames, and the game waits forever for its previous present to be processed before "acquiring the next swapchain texture"):
// when mtl_present_wait has waited longer than STALL_NS and the display link has not called back for that long, drop the waiting frame and earlier queued frames as when entering the background (accounted as dropped frames) and let the game keep running; when the display link comes back things return to normal automatically.
// One stall logs one hint line, and one line on recovery (neither is a failure); only when the same stall lasts longer than STALL_RED_NS is it a failure. All under g_pq_mu
#define STALL_NS 100000000ull
#define STALL_RED_NS 2000000000ull
static uint64_t g_stall_t0, g_stall_drop, g_stall_n; static int g_stall_red, g_stall_logged;
#define LINK_RESCUE_NS 500000000ull
static int g_rescue_mode; static uint64_t g_rescue_back_at, g_rescue_back_ns = 3000000000ull, g_rescue_n;
// The display link watchdog only recognizes the "active" state: when the App loses active status (pulling down Control Center / notifications, incoming call, system popup) and in the background the system does not call back anyway, and the picture is covered too -- the frame-drop fallback stays as before,
// but it is not counted as stuck and no failure is reported; the stall is measured from the later of "the most recent callback / the most recent activation" (A16 logs: 8–17 seconds inactive and 17 seconds in the background were both reported as 「前台卡住」(stuck in foreground))
__attribute__((weak)) int dm_focus_now(void) { return 1; }   // The host's hle_oe.c provides the strong definition; when graphics unit tests do not link the host it is treated as always active
__attribute__((weak)) uint64_t dm_focus_in_since(void) { return 0; }
@implementation NvMetalDisplayLinkDelegate
- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update {
    (void)link;
    pthread_mutex_lock(&g_pq_mu);
    if (g_stall_t0) { uint64_t since = dm_focus_in_since(), t = now_ns();
        MLOG("显示节拍恢复：这段停了 %.0f ms（其中激活中 %.0f ms），兜底丢了 %llu 帧（第 %llu 段）", (t - g_stall_t0) / 1e6, (t - (g_stall_t0 > since ? g_stall_t0 : since)) / 1e6, (unsigned long long)g_stall_drop, (unsigned long long)g_stall_n); g_stall_t0 = 0; }
    pthread_mutex_unlock(&g_pq_mu);
    pthread_mutex_lock(&g_link_mu);
    g_link_tick_ns = now_ns();
    g_link_tick_n++;
    pthread_cond_broadcast(&g_link_cv);
    pthread_mutex_unlock(&g_link_mu);
    pthread_mutex_lock(&g_pq_mu);
    if (!g_dm_present_link || g_pq_read == g_pq_in) { pthread_mutex_unlock(&g_pq_mu); return; }
    // The previous frame's present interval was >= 2 (period > 25 ms): keep it on screen for its whole period before taking the next one, as the Switch flips only every N vertical syncs. This also holds while the link runs at 60 Hz (Mario 1.2.1 sets 2 for single frames, see nvn_game_fps in hle_nvn.c);
    //   before, every tick took the newest frame, so that frame, carrying the extra time step the game advanced for it, was shown for only 16.7 ms and the game looked like it jumped ahead
    if (g_link_hold_until > 0 && update.targetPresentationTimestamp < g_link_hold_until - 0.002) { g_link_held++; pthread_mutex_unlock(&g_pq_mu); return; }
    uint64_t first = g_pq_read + 1, ticket = g_pq_in;
    // In order, one frame per tick and at most one behind the newest: two waiting, the older now and the newer on the next tick; three or more, the older ones are dropped. Taking the newest
    // dropped the one before whenever two frames landed in the same refresh, as frames of a 60 fps game that take 16-17.5 ms do, and the next frame then stayed up 33 ms
    // (Dark Souls Remastered, from PR #31: presents within 14.7-18.7 ms 98.2 -> 99.98 %, about 2 ms more from enqueue to display). DM_PRESENT_FIFO=0 takes the newest, as before
    static int fifo = -1; if (fifo < 0) { const char *e = getenv("DM_PRESENT_FIFO"); fifo = !(e && !strcmp(e, "0")); MLOG("display link: %s", fifo ? "frames in order, at most one behind" : "the newest frame each tick (DM_PRESENT_FIFO=0)"); }
    if (fifo && ticket > first) ticket = ticket - first >= 2 ? g_pq_in - 1 : first;
    pq_item it = g_pq[(ticket - 1) % PQ]; g_link_it_tin = it.t_in;
    g_pq_read = ticket;
    if (ticket > first) { g_link_dropped += ticket - first; for (uint64_t k = first; k < ticket; k++) g_pres[k % PRING] = (pres_ent){k, -1}; }   // Old frames merged and dropped are recorded as "not presented", so the present statistics honestly count them as dropped frames
    if (ticket > 1 && g_pq_done < ticket - 1) g_pq_done = ticket - 1;
    pthread_cond_broadcast(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
    int submitted;
    @autoreleasepool { submitted = present_now(&it.t, it.period, it.frame, ticket, it.crop, update.drawable, update.targetPresentationTimestamp,it.t_in); }
    g_link_hold_until = submitted && it.period > 0.025 ? update.targetPresentationTimestamp + it.period : 0;
    if (!submitted) {
        pthread_mutex_lock(&g_pq_mu);
        if (ticket > g_pq_done) g_pq_done = ticket;
        pthread_cond_broadcast(&g_pq_cv);
        pthread_mutex_unlock(&g_pq_mu);
    }
}
@end

static CAMetalDisplayLink *g_link; static CFRunLoopRef g_link_rl; static int g_link_started;
static int link_on(void) { return g_dm_present_link && g_link != nil; }
static CAFrameRateRange link_range(int mode) { return mode == 3 ? CAFrameRateRangeMake(30, 30, 30) : mode == 2 ? CAFrameRateRangeMake(60, 120, 120) : CAFrameRateRangeMake(60, 60, 60); }
static int g_link_mode = 1;   // Current mode of the display link object: 1 / 2 same as g_dm_present_link, 3 = power-saving mode 30 Hz
static void *link_main(void *arg) {   // Display link thread: started when switching to display-link mode, exits when switching back to 0
    (void)arg;
    pthread_setname_np("nvn-present-link");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    CAMetalLayer *layer = (__bridge CAMetalLayer *)front_metal_layer();
    pthread_mutex_lock(&g_pq_mu); g_link_owns_layer = 1; while (g_classic_pending) pthread_cond_wait(&g_pq_cv, &g_pq_mu); pthread_mutex_unlock(&g_pq_mu);   // Exclude new nextDrawable calls, then wait for every scheduled classic present to return before attaching
#if TARGET_OS_OSX
    if (layer && !layer.displaySyncEnabled) layer.displaySyncEnabled = YES;
#endif
    NvMetalDisplayLinkDelegate *delegate = [NvMetalDisplayLinkDelegate new];
    CAMetalDisplayLink *link = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
    link.preferredFrameRateRange = link_range(g_link_mode);
    link.preferredFrameLatency = g_present_latency;
    link.delegate = delegate;
    [link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
    g_link_rl = CFRunLoopGetCurrent(); g_link = link;
    CFRunLoopRun();   // When switching back to mode 0 the display link object is invalidated and this thread exits (nextDrawable cannot get a drawable while the layer still has the display link object attached, so pausing alone is not enough)
    g_link_started = 0;
    return NULL;
}
static void link_sync(void) {   // Called every frame by the game's present thread: aligns the display link object's state to g_dm_present_link and the power-saving mode (can change at run time)
    static int applied; int want = g_dm_present_link;
    if (g_rescue_mode && now_ns() >= g_rescue_back_at) {
        pthread_mutex_lock(&g_pq_mu); g_dm_present_link = g_rescue_mode; g_rescue_mode = 0; g_stall_t0 = 0; g_link_tick_ns = now_ns(); pthread_mutex_unlock(&g_pq_mu);
        MLOG("显示节拍：抢救期满，换回节拍模式 %d 再试（帧 %llu）", g_dm_present_link, (unsigned long long)atomic_load(&g_cur_frame));
    }
    if (want < 0 || want > 2) { g_dm_present_link = applied == 3 ? 1 : applied; return; }
    if (want && mtl_fps_target() == 30) want = 3;   // Power-saving mode: display link at 30 Hz
    if (want == applied) return;
    g_link_mode = want;
    if (!g_link_started) {
        if (!want) { applied = 0; return; }
        if (!front_metal_layer()) return;
        g_link_started = 1; pthread_t th; if (pthread_create(&th, NULL, link_main, NULL)) MDIE("显示节拍线程起不来"); pthread_detach(th);
        return;   // The display link object is created on a new thread; align again next frame
    }
    CAMetalDisplayLink *link = g_link; if (!link) return;
    if (want) link.preferredFrameRateRange = link_range(want);
    else { CFRunLoopRef rl = g_link_rl; g_link = nil; CFRunLoopPerformBlock(rl, kCFRunLoopDefaultMode, ^{ [link invalidate]; pthread_mutex_lock(&g_pq_mu); g_link_owns_layer = 0; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu); CFRunLoopStop(rl); }); CFRunLoopWakeUp(rl); }
    MLOG("出图节拍模式 %d → %d（帧 %llu）", applied, want, (unsigned long long)atomic_load(&g_cur_frame));
    applied = want;
    pthread_mutex_lock(&g_pq_mu); pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
}
#else
// CAMetalDisplayLink first exists on iOS 17. iOS 16.1 uses the classic
// present thread and command-buffer presentDrawable path.
#define STALL_NS 100000000ull
#define STALL_RED_NS 2000000000ull
#define LINK_RESCUE_NS 500000000ull
static uint64_t g_stall_t0, g_stall_drop, g_stall_n;
static int g_stall_red, g_stall_logged;
static int g_rescue_mode;
static uint64_t g_rescue_back_at, g_rescue_back_ns = 3000000000ull, g_rescue_n;
__attribute__((weak)) int dm_focus_now(void) { return 1; }
__attribute__((weak)) uint64_t dm_focus_in_since(void) { return 0; }
static int g_link_mode;
static int link_on(void) { return 0; }
static void link_sync(void) { g_dm_present_link = 0; g_link_mode = 0; }
#endif
// Before going to the background (mtl_drain): when the display link drives presents, frames queued but not yet presented can only be taken by the display-link callback, and in the background the system no longer calls back --
// waiting as-is for "the present queue to empty" would wait forever, the enter-background callback would not return, and the system would kill the process (the process disappears within 22 seconds after switching to another App).
// These frames will not be seen anyway: discard them directly (accounted as dropped frames) and only wait for the one present already submitted to the GPU to finish.
static void present_drop_for_suspend(void) {
    pthread_mutex_lock(&g_pq_mu);
    if (g_link_owns_layer) {
        uint64_t n = g_pq_in - g_pq_read;
        for (uint64_t k = g_pq_read + 1; k <= g_pq_in; k++) g_pres[k % PRING] = (pres_ent){k, -1};
        g_link_dropped += n; g_pq_read = g_pq_in;
        while (g_pq_inflight) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        if (g_pq_done < g_pq_in) g_pq_done = g_pq_in;
        pthread_cond_broadcast(&g_pq_cv);
        if (n) MLOG("进后台：丢弃排队中未上屏的 %llu 帧（显示节拍在后台不回调）", (unsigned long long)n);
    }
    pthread_mutex_unlock(&g_pq_mu);
}
static void *present_main(void *arg) {
    (void)arg;
    pthread_setname_np("nvn-present");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0; pthread_get_qos_class_np(pthread_self(), &qc, &rp); atomic_store(&g_present_qos, (uint32_t)qc);
    for (;;) {
        pthread_mutex_lock(&g_pq_mu);
        while (g_pq_read == g_pq_in || g_link_owns_layer) pthread_cond_wait(&g_pq_cv, &g_pq_mu);   // In display-link mode the display-link callback takes them; an empty queue is judged by "taken"; "done" has to wait for the feedback of the present submit and lags behind taken
        pq_item it = g_pq[g_pq_read % PQ];
        uint64_t ticket = ++g_pq_read; g_classic_pending++;
        pthread_mutex_unlock(&g_pq_mu);
        int submitted;
        @autoreleasepool { submitted = present_now(&it.t, it.period, it.frame, ticket, it.crop, nil, 0,it.t_in); }
        if (!submitted) {
            classic_present_done();   // No scheduled callback owns this failed frame
            pthread_mutex_lock(&g_pq_mu);
            if (ticket > g_pq_done) g_pq_done = ticket;
            pthread_cond_broadcast(&g_pq_cv);
            pthread_mutex_unlock(&g_pq_mu);
        }
    }
    return NULL;
}
uint32_t mtl_present_qos(void) { return atomic_load(&g_present_qos); }
static void present_start(void) { pthread_t th; if (pthread_create(&th, NULL, present_main, NULL)) MDIE("出图线程起不来"); pthread_detach(th); }
static _Atomic int g_present_discard;
void mtl_link_pause(int on);
// The short period when the front end goes to the background and waits for the game to save: presents are dropped directly (the display link does not call back in the background, enqueueing would block the game thread; nobody sees the picture anyway).
// When turned on, also clear the frames already queued: the system first stops the display-link callback and only afterwards notifies about going to the background, and in that gap the game has already filled the swapchain and is stuck at "before acquiring the next texture, wait for its previous present to be processed"
// (hle_nvn.c nvnWindowAcquireTexture -> mtl_present_wait). Without clearing, the game cannot advance a single tick during those 2.5 seconds of waiting for the save, and the save is not written (the same pattern three times in player logs: 3 frames queued, 0 submits)
void mtl_present_discard(int on) { atomic_store(&g_present_discard, on != 0); if (on) present_drop_for_suspend(); }
uint64_t mtl_present(const tex_t *t, double period, const int crop[4]) {
    if (atomic_load(&g_present_discard)) return 0;   // Ticket number 0 = no pending present; the swapchain texture acquire does not wait
    if (t) { xv_check(t->serial, 0, 0, "出图"); if (g_xv_npresent < 128) g_xv_present[g_xv_npresent++] = t->serial; }
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, present_start);
    link_sync();
    {   // Test switch DM_TEST_LINK_STALL=start seconds,duration seconds: counting from the first present, pause the display link at that point (imitating the display link not calling back while in the foreground) and release it when the duration is up. For verifying the fallback; not set in the product
        static double t0, at = -1, dur; static int st; double now = CACurrentMediaTime();
        if (at < 0) { at = 0; const char *e = getenv("DM_TEST_LINK_STALL"); if (e && sscanf(e, "%lf,%lf", &at, &dur) == 2 && at > 0 && dur > 0) { t0 = now; st = 1; } else at = 0; }
        if (st == 1 && link_on() && now - t0 >= at) { st = 2; mtl_link_pause(1); MLOG("DM_TEST_LINK_STALL：暂停显示节拍 %.1f 秒（帧 %llu）", dur, (unsigned long long)atomic_load(&g_cur_frame)); }
        else if (st == 2 && now - t0 >= at + dur) { st = 3; mtl_link_pause(0); MLOG("DM_TEST_LINK_STALL：放开显示节拍（帧 %llu）", (unsigned long long)atomic_load(&g_cur_frame)); }
    }
    pthread_mutex_lock(&g_pq_mu);
    while (g_pq_in - g_pq_done >= PQ) pthread_cond_wait(&g_pq_cv, &g_pq_mu);   // Wait only when the queue is full; normally AcquireTexture blocks first
    pq_item *it = &g_pq[g_pq_in % PQ]; *it = (pq_item){*t, period, atomic_load(&g_cur_frame), {0}, CACurrentMediaTime()};
    if (crop) memcpy(it->crop, crop, sizeof it->crop);   // The current frame's state is enqueued with the ticket number, rather than reading the mutable window late on the present thread
    uint64_t tk = ++g_pq_in;
    pthread_cond_broadcast(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
    return tk;
}

// Bridge used by the Vulkan/OpenGL front ends that are linked into the common
// host. Z-A itself uses NVN, but the symbols remain part of the shared host.
void *mtl_device(void) { mtl_init(); return (__bridge void *)g_dev; }
uint64_t mtl_ext_tex(void *tex) {
    static _Atomic uint64_t n;
    id<MTLTexture> t = (__bridge id<MTLTexture>)tex;
    uint64_t serial = (1ull << 62) + atomic_fetch_add(&n, 1) + 1;
    res_add(t); tex_put(serial, t);
    return serial;
}
uint64_t mtl_present_ext(uint64_t serial, double period, void *event, uint64_t event_value, const int crop[4]) {
    (void)event; (void)event_value;
    tex_t t; memset(&t, 0, sizeof t); t.magic = MAGIC('T', 'E', 'X', 0); t.serial = serial;
    return mtl_present(&t, period, crop);
}
// Do not wait during discard: the frame enqueued only after the flag was set (the present thread had just passed the flag check) has nobody to take it, and waiting for it would stall again; it is left for the following mtl_drain to clear
// Do not wait indefinitely in display-link mode: see the note at STALL_NS. Only frames "not yet taken by the display link" are dropped; the one already on its way to the screen is waited for until handed to the GPU (same dropping as when entering the background)
void mtl_present_wait(uint64_t tk) {
    pthread_mutex_lock(&g_pq_mu);
    while (g_pq_done < tk && !atomic_load(&g_present_discard)) {
        if (!g_link_owns_layer) { pthread_cond_wait(&g_pq_cv, &g_pq_mu); continue; }
        if (g_pq_read >= tk) { pthread_cond_wait(&g_pq_cv, &g_pq_mu); continue; }   // This one has already been taken by the display link and is being presented: wait for it to be handed to the GPU (the GPU completion callback will wake us)
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += (long)STALL_NS; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        // Already determined to be in a stall (when the display link comes back the callback first clears g_stall_t0, then records the display-link tick time): no longer wait 100 ms in vain every frame, drop directly, and the game proceeds by its own display-link wait timeout (about two ticks)
        if (!g_stall_t0 && (pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts) != ETIMEDOUT || g_pq_done >= tk)) continue;
        uint64_t now = now_ns(), last = __atomic_load_n(&g_link_tick_ns, __ATOMIC_RELAXED);
        if (g_pq_read >= tk) continue;
        if (now - last < STALL_NS) { pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts); continue; }   // The display link is still running, it is just not this one's turn yet: keep waiting (bounded, no busy spinning)
        uint64_t n = tk - g_pq_read;
        for (uint64_t k = g_pq_read + 1; k <= tk; k++) g_pres[k % PRING] = (pres_ent){k, -1};
        g_link_dropped += n; g_pq_read = tk;
        while (g_pq_inflight) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        if (g_pq_done < tk) g_pq_done = tk;
        pthread_cond_broadcast(&g_pq_cv);
        if (!g_stall_t0) { g_stall_t0 = last ? last : now; g_stall_drop = 0; g_stall_red = 0; g_stall_logged = 0; g_stall_n++; }
        g_stall_drop += n;
        int active = dm_focus_now() == 1; uint64_t since = dm_focus_in_since(), from = g_stall_t0 > since ? g_stall_t0 : since;
        if (active && !g_stall_logged && now - from >= STALL_NS) { g_stall_logged = 1;
            MLOG("显示节拍 %.0f ms 没回调（激活中）：丢掉排队未上屏的 %llu 帧，游戏继续（第 %llu 段；节拍回来自动照常）", (now - from) / 1e6, (unsigned long long)n, (unsigned long long)g_stall_n); }
        if (active && !g_rescue_mode && g_dm_present_link && now - from >= LINK_RESCUE_NS) {
            g_rescue_mode = g_dm_present_link; g_dm_present_link = 0; g_rescue_n++; g_rescue_back_at = now + g_rescue_back_ns;
            MLOG("显示节拍停滞超过 %.0f ms（激活中）：自动抢救切换至传统模式，画面保持刷新（第 %llu 次）", (now - from) / 1e6, (unsigned long long)g_rescue_n);
        }
        if (active && !g_stall_red && now - from >= STALL_RED_NS) { g_stall_red = 1; MLOG("【红】显示节拍激活中连续 %.1f 秒没回调，出图全靠兜底丢帧，画面不动", (now - from) / 1e9); }
    }
    pthread_mutex_unlock(&g_pq_mu);
}
// Wait until all queued presents have been processed (used by mtl_finish = nvnQueueFinish etc.): goes through mtl_present_wait, so there is still a fallback when the display link stops (real device: the game called QueueFinish once while the display link was stopped,
// and this used to wait here without a timeout; nobody took the 3 queued frames and the game stalled; those two runs on Mac did not hit this call)
void mtl_present_flush(void) { pthread_mutex_lock(&g_pq_mu); uint64_t h = g_pq_in; pthread_mutex_unlock(&g_pq_mu); mtl_present_wait(h); }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = g_stat_pass; *draws = g_stat_draw; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = g_stat_args_enc; *reuse = g_stat_args_reuse; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = g_legacy_use_calls; *skip = g_legacy_use_skipped; }
void mtl_state_stats(uint64_t *call, uint64_t *skip) { *call = g_legacy_state_calls; *skip = g_legacy_state_skips; }
void mtl_pipeline_bind_stats(uint64_t *pso_call, uint64_t *pso_skip, uint64_t *dss_call, uint64_t *dss_skip) {
    *pso_call = g_legacy_pso_calls; *pso_skip = g_legacy_pso_skips;
    *dss_call = g_legacy_dss_calls; *dss_skip = g_legacy_dss_skips;
}
void mtl_buffer_bind_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *offset) {
    *resolve = g_vbuf_resolve; *reuse = g_vbuf_reuse; *offset = g_buffer_offset_calls;
}
void mtl_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip, uint64_t *copies, uint64_t *copy_bytes) {
    *resolve = g_vbuf_resolve; *reuse = g_vbuf_reuse; *set = 0; *skip = 0; *copies = 0; *copy_bytes = 0;
}
uint64_t mtl_copy_cut_count(void) { return 0; }
void mtl_lc_stats(uint64_t *taken, uint64_t *landed) { *taken = 0; *landed = 0; }
void mtl_ubp_stats(uint64_t *made, uint64_t *reused, uint64_t *refused) { *made = 0; *reused = 0; *refused = 0; }
void mtl_ubp_why(uint64_t why[5]) { memset(why, 0, 5 * sizeof *why); }
uint64_t mtl_ds_omit(void) { return g_stat_ds_omit; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = g_bw_load; *store = g_bw_store; *full = g_pass_full_rw; }
uint64_t mtl_udc_saved_bytes(void) { return g_udc_saved_bytes; }
int mtl_has_window(void) { return front_metal_layer() != NULL; }
int mtl_link_active(void) { return link_on(); }
#if DM_HAVE_METAL_DISPLAY_LINK
void mtl_link_pause(int on) { CAMetalDisplayLink *l = g_link; if (l) l.paused = on != 0; }   // For testing (DM_TEST_BG): imitates the system stopping the display-link callback in the background
#else
void mtl_link_pause(int on) { (void)on; }
#endif
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) {
    if (!mtl_link_active()) return 0;
    // Wait for the display link only when running faster than the display: if a display-link tick has already passed since the previous frame started work (this frame is already behind), start immediately instead of waiting in vain for the next display-link tick --
    // otherwise when the frame time is slightly more than one display-link period it would "miss the tick -> wait another whole tick", locking the frame rate straight to half (measured on Mac under windowed load: runs of dropped frames)
    static uint64_t started_tick;   // Display-link tick number when the previous frame started work (only the game's present thread calls this function)
    // How many display-link ticks one frame takes: the game's present interval (vsyncs, 60 Hz) times 1 with a 60 Hz display link, times 2 with a 120 Hz one, halved and rounded up at 30 Hz.
    // A frame presented with interval N blocks the game for N vsyncs on the Switch, as the display callback holds it N ticks: games that scale their time step by the interval (BotW's VFR: interval / 2)
    // ran at double speed while they produced interval-2 frames every 60 Hz tick (iPhone log: Link sprinting with B, the interval flapping 1 ↔ 2)
    if (vsyncs < 1) vsyncs = 1;
    const uint64_t step = g_link_mode == 3 ? (uint64_t)(vsyncs + 1) / 2 : (uint64_t)vsyncs * (g_link_mode == 2 ? 2 : 1);
    pthread_mutex_lock(&g_link_mu);
    if (g_link_tick_n >= started_tick + step) { started_tick = g_link_tick_n; pthread_mutex_unlock(&g_link_mu); return 1; }
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ns / 1000000000ull; ts.tv_nsec += timeout_ns % 1000000000ull;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (g_link_tick_n < started_tick + step) if (pthread_cond_timedwait(&g_link_cv, &g_link_mu, &ts) == ETIMEDOUT) { pthread_mutex_unlock(&g_link_mu); return 0; }
    started_tick = g_link_tick_n;
    pthread_mutex_unlock(&g_link_mu); return 1;
}

// The display's vsync for vi::GetDisplayVsyncEvent (hle_sdk16.c): returns 1 at the first display-link tick of the next 60 Hz vsync after *seen (the tick count it last returned, updated),
// 0 when the link does not run, runs at 30 Hz (power saving: the 60 Hz event cannot come from it) or gives no tick within the timeout; the caller then keeps its own clock
int mtl_link_wait_vsync(uint64_t *seen, uint64_t timeout_ns) {
    if (!mtl_link_active() || g_link_mode == 3) return 0;
    const uint64_t step = g_link_mode == 2 ? 2 : 1;
    pthread_mutex_lock(&g_link_mu);
    if (*seen > g_link_tick_n || g_link_tick_n > *seen + 4 * step) *seen = g_link_tick_n;   // first call, or far behind (a stall): from the current tick on, no burst
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ns / 1000000000ull; ts.tv_nsec += timeout_ns % 1000000000ull;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (g_link_tick_n < *seen + step) if (pthread_cond_timedwait(&g_link_cv, &g_link_mu, &ts) == ETIMEDOUT) { pthread_mutex_unlock(&g_link_mu); return 0; }
    *seen += step;
    pthread_mutex_unlock(&g_link_mu); return 1;
}

static id<CAMetalDrawable> g_prefetched_drawable; // Used for compositing straight onto the drawable
id<CAMetalDrawable> mtl_present_take_prefetched(void) { return g_prefetched_drawable; }

#ifdef NVN_RT_RANGE_TEST
#include "rt_range_check.inc"
#endif

// Called by the watchdog (no progress): writes the state of all blocking wait points on the graphics side to the log -- who is waiting for whom is visible at a glance (after switching to the Metal 4 submit model there was once a case of "ran to 900 frames then stopped, no failure line")
void mtl_wait_state_dump(void) {
    MLOG("等待点状态：主栅栏 已提交 %llu / 已完成 %llu；主分配器栅栏 [%llu %llu %llu]；上传 未完成 %d、分配器占用 [%d %d %d]、序号 %u、有未冲刷批次 %d；出图队列 入 %llu / 完成 %llu；主命令缓冲在编 %d、提交线程已设 %d",
         (unsigned long long)g_fence, (unsigned long long)g_done, (unsigned long long)g_alloc_fence[0], (unsigned long long)g_alloc_fence[1], (unsigned long long)g_alloc_fence[2],
         atomic_load(&g_up_pending), atomic_load(&g_up_alloc_busy[0]), atomic_load(&g_up_alloc_busy[1]), atomic_load(&g_up_alloc_busy[2]), g_up_seq, g_up_cb != nil,
         (unsigned long long)g_pq_in, (unsigned long long)g_pq_done, g_cb != nil, atomic_load(&g_sub_thread_set));
}
void mtl_alloc_stats(uint64_t *waits, uint64_t *grown, int *n) { *waits = g_alloc_waits; *grown = g_alloc_grown; *n = g_alloc_n; }   // Main submit allocator pool: how many times it waited, how many times it grew, how many exist now

// Live pass metadata is bounded and lock-free. Pixel / raw-texture control
// captures remain inert until their implementations are ported.
void dm_gfx_passdump_next(long n) {
    if (n <= 0) { atomic_store_explicit(&g_passdump_live, 0, memory_order_release); return; }
    uint64_t start = atomic_load(&g_cur_frame) + 1, count = n > 16 ? 16 : (uint64_t)n;
    if (start > (UINT64_MAX >> 8)) return;
    atomic_store_explicit(&g_passdump_live, (start << 8) | count, memory_order_release);
    MLOG("Live pass metadata: frames %llu..%llu (%llu frames)", (unsigned long long)start,
         (unsigned long long)(start + count - 1), (unsigned long long)count);
}
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_texlist(int target) { (void)target; }
void dm_gfx_texraw(uint64_t serial, const char *path) { (void)serial; (void)path; }
#pragma clang attribute pop
