// The Vulkan backend's command stream (docs/design/vulkan-backend.md 4.5-4.9 and the per-operation group of nvn_be.h): submissions, render passes, draws, dispatches, clears, copies,
// barriers, events and syncs, counter reports and queries, conditional rendering and transform feedback.
// Submissions (4.5): one queue; a submission is a prologue command buffer (texture uploads and zero-fills, which run first) and its command buffer, submitted together with a
// timeline semaphore signalled at the submission's fence value. Eight submission slots each own their command pool, ring chunks, descriptor pools and query pools, recycled when
// their fence has passed. A completion thread waits on the timeline in order and runs each finished submission's CPU work (events, the counter reports the GPU does not write,
// deferred releases), then publishes the fence: be_wait waits for that publication.
// Ordering (4.9): a full memory barrier before every transfer, dispatch and render pass that follows anything recorded since the last one; inside a pass, draws are ordered by
// rasterization order, and a game barrier after storage writes ends the pass first (as the Metal backend's swrite_note).
// Render passes: dynamic rendering, begun at the first draw or clear after SetRenderTargets; full clears before it become load operations (folded clears, as Metal's load
// actions); a clear inside a pass, with a scissor or under a condition is vkCmdClearAttachments; one with a partial channel or stencil mask is the clear triangle (2.7, 5.5).
// Counters (4.6): samples passed from precise occlusion queries, one per pass segment, timestamps from vkCmdWriteTimestamp2; at the next point outside a pass a kernel adds them up,
// converts the timestamp to NVN's 614.4 MHz units on the guest clock (VK_KHR_calibrated_timestamps) and writes the 16-byte report into guest memory, in order on the GPU, so a
// later command of the same submission reads it (the Metal backend's CPU reports cannot). Conditional rendering (4.7): a one-thread kernel compares the two counters into a
// predicate word that the following passes test (VK_EXT_conditional_rendering).
// Declared with a red line: indirect quads and fans, and indices or vertices the GPU writes earlier in the same submission that need a CPU conversion; other counter types
// (pipeline statistics, primitives generated: phase P8); the alpha test; advanced blend modes; viewport swizzles other than a negated y; presentation (phase P4).
// Pipelines (phase P5): the draw fills its key through vk_gpl.c's hvk_key_fill and takes its pipeline from hvk_gfx_pipeline, which counts the time a draw waits for it; the first
// submission starts the prewarm (vk_prewarm.c), as the Metal backend's does, since the vertex stage's depth mode is the game's by then.
#include "plat/plat.h"
#include "vk_backend.h"
#include "vk_pso.h"
#include "../nvn/nvn_state.h"
#include "../nvn/nvn_resolve.h"
#include "../nvn/nvn_gpuwrite.h"
#include "../nvn/nvn_primitive.h"
#include "../nvn/nvn_timestamp.h"
#include "../nvn/nvn_replay.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---------- Submission slots ----------
enum { NSLOT = 8, NOCC = 4096, NTS = 1024 };
typedef enum { POST_EVENT, POST_COUNTER } post_kind;
typedef struct { post_kind kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; } post_op;
typedef struct {
    VkCommandPool pool; VkCommandBuffer pre, main; int pre_begun;
    uint64_t fence;            // the fence of the submission it holds, 0 when free
    hvk_chunk *chunks;
    VkQueryPool occ, ts; uint32_t nocc, nts;
    post_op *posts; int nposts, cposts;
} slot_t;
static slot_t g_slot[NSLOT];
static int g_cur = -1, g_next;
static VkSemaphore g_timeline;
_Atomic uint64_t hvk_last_fence;           // the last fence committed
static _Atomic uint64_t g_done;            // the last fence whose CPU work has run
static uint64_t g_open;                    // the open submission's fence (0 = none)
static pthread_mutex_t g_done_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_done_cv = PTHREAD_COND_INITIALIZER, g_work_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_thread; static int g_thread_on, g_quit;
static int g_need_barrier;
uint64_t hvk_open_fence(void) { return g_open; }
uint64_t hvk_done_fence(void) { return atomic_load(&g_done); }
int hvk_slot_index(void) { return g_cur < 0 ? 0 : g_cur; }
void hvk_need_barrier(void) { g_need_barrier = 1; }
void hvk_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL};
    vkCmdPipelineBarrier2(cb, &di);
    atomic_fetch_add(&hvk.st_barriers, 1);
}
typedef struct pass_t pass_t;
static int pass_active(void);
void hvk_barrier_if_needed(VkCommandBuffer cb) { if (g_need_barrier && !pass_active()) { hvk_full_barrier(cb); g_need_barrier = 0; } }
VkCommandBuffer hvk_main(void) { return g_cur >= 0 ? g_slot[g_cur].main : VK_NULL_HANDLE; }
VkCommandBuffer hvk_pre(void) {
    if (g_cur < 0) return VK_NULL_HANDLE;
    slot_t *s = &g_slot[g_cur];
    if (!s->pre_begun) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(s->pre, &bi);
        hvk_full_barrier(s->pre);   // after the submissions before it (an upload may replace what an earlier one still reads)
        s->pre_begun = 1;
    }
    return s->pre;
}
static void post_add(post_op o) {
    slot_t *s = &g_slot[g_cur];
    if (s->nposts == s->cposts) { s->cposts = s->cposts ? 2 * s->cposts : 64; s->posts = realloc(s->posts, (size_t)s->cposts * sizeof *s->posts); if (!s->posts) { s->cposts = s->nposts = 0; return; } }
    s->posts[s->nposts++] = o;
}
static uint64_t ts_now_nvn(void) { uint64_t g = guest_ns(); return nvn_timestamp_from_ns(g ? g : plat_now_ns()); }
static void post_run(slot_t *s) {
    for (int i = 0; i < s->nposts; i++) {
        post_op *o = &s->posts[i];
        if (o->kind == POST_EVENT) { dm_guest_write_prepare((void *)(uintptr_t)o->val, 4); if (o->mode == NVN_EVENT_SIGNAL_MODE_ADD) *o->val += o->v; else *o->val = o->v; }
        else { uint64_t rec[2] = {0, ts_now_nvn()}; dm_guest_write_prepare((void *)(uintptr_t)o->addr, 16); memcpy((void *)(uintptr_t)o->addr, rec, 16); }
    }
    s->nposts = 0;
}
// The completion thread: in fence order, wait on the timeline, run the slot's CPU work, publish
static void *completion(void *arg) {
    (void)arg; plat_thread_set_name("hostvk completion");
    for (;;) {
        pthread_mutex_lock(&g_done_mu);
        while (!g_quit && atomic_load(&g_done) >= atomic_load(&hvk_last_fence)) pthread_cond_wait(&g_work_cv, &g_done_mu);
        if (g_quit && atomic_load(&g_done) >= atomic_load(&hvk_last_fence)) { pthread_mutex_unlock(&g_done_mu); break; }
        uint64_t f = atomic_load(&g_done) + 1;
        pthread_mutex_unlock(&g_done_mu);
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &g_timeline, &f};
        while (vkWaitSemaphores(hvk.d, &wi, 1000000000ull) == VK_TIMEOUT) {}
        for (int i = 0; i < NSLOT; i++) if (g_slot[i].fence == f) { post_run(&g_slot[i]); break; }
        hvk_deferred_run(f);
        pthread_mutex_lock(&g_done_mu); atomic_store(&g_done, f); pthread_cond_broadcast(&g_done_cv); pthread_mutex_unlock(&g_done_mu);
    }
    return NULL;
}
int hvk_wait(uint64_t fence, uint64_t timeout_ns) {
    if (atomic_load(&g_done) >= fence) return 1;
    if (!timeout_ns) return 0;
    uint64_t t0 = plat_now_ns(), limit = timeout_ns > UINT64_MAX - t0 ? UINT64_MAX : t0 + timeout_ns;
    pthread_mutex_lock(&g_done_mu);
    while (atomic_load(&g_done) < fence) {
        uint64_t now = plat_now_ns();
        if (now >= limit) break;
        uint64_t left = limit - now; if (left > 100000000ull) left = 100000000ull;   // a bounded slice: the clock of pthread_cond_timedwait is the realtime one
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t ns = (uint64_t)ts.tv_nsec + left; ts.tv_sec += (time_t)(ns / 1000000000ull); ts.tv_nsec = (long)(ns % 1000000000ull);
        pthread_cond_timedwait(&g_done_cv, &g_done_mu, &ts);
    }
    int ok = atomic_load(&g_done) >= fence;
    pthread_mutex_unlock(&g_done_mu);
    return ok;
}

// One-off command buffers (CPU reads of GPU textures): their own pool, a fence, the queue lock
static VkCommandPool g_oneoff_pool; static VkFence g_oneoff_fence; static pthread_mutex_t g_oneoff_mu = PTHREAD_MUTEX_INITIALIZER;
int hvk_oneoff_begin(VkCommandBuffer *cb);
int hvk_oneoff_begin(VkCommandBuffer *cb) {
    pthread_mutex_lock(&g_oneoff_mu);
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_oneoff_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vkAllocateCommandBuffers(hvk.d, &ai, cb)) { pthread_mutex_unlock(&g_oneoff_mu); return 0; }
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(*cb, &bi);
    return 1;   // g_oneoff_mu stays held until hvk_oneoff_end
}
int hvk_oneoff_end(VkCommandBuffer cb);
int hvk_oneoff_end(VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkCommandBufferSubmitInfo ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, cb, 0};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, 1, &ci, 0, NULL};
    vkResetFences(hvk.d, 1, &g_oneoff_fence);
    pthread_mutex_lock(&hvk.queue_mu); VkResult r = vkQueueSubmit2(hvk.dev.queue, 1, &si, g_oneoff_fence); pthread_mutex_unlock(&hvk.queue_mu);
    if (!r) r = vkWaitForFences(hvk.d, 1, &g_oneoff_fence, VK_TRUE, UINT64_MAX);
    vkFreeCommandBuffers(hvk.d, g_oneoff_pool, 1, &cb);
    pthread_mutex_unlock(&g_oneoff_mu);
    return r == VK_SUCCESS;
}

// ---------- Counters (4.6): the occlusion accumulator, the calibration ----------
static VkBuffer g_acc; static VkDeviceMemory g_acc_mem; static VkDeviceAddress g_acc_dev;   // the samples-passed count folded so far (64 bits)
static int g_counting, g_acc_valid;        // a ResetCounter(SAMPLES_PASSED) was seen; g_acc holds the count up to g_occ_first (else it is taken as 0)
static uint32_t g_occ_first;               // the first query of this slot not yet added into g_acc
static uint64_t g_cal_mul, g_cal_g0, g_cal_n0;   // NVN ticks = n0 + ((gpu ticks - g0) * mul >> 32)
typedef struct { uint64_t dst; uint32_t q0, nq, ts, flags; } report_t;   // flags: 1 samples passed, 2 use the accumulator, 4 store into the accumulator, 8 timestamp counter value
static report_t g_rep[64]; static int g_nrep;
static void calibrate(void) {
    double period = hvk.lim.timestampPeriod > 0 ? hvk.lim.timestampPeriod : 1;
    g_cal_mul = (uint64_t)(period * 384.0 / 625.0 * 4294967296.0);
    uint64_t gpu = 0, host = 0; int ok = 0;
    if (hvk.dev.x.calibrated) {
#if defined(_WIN32)
        VkTimeDomainKHR hd = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
#else
        VkTimeDomainKHR hd = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
#endif
        VkCalibratedTimestampInfoKHR ci[2] = {{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, NULL, VK_TIME_DOMAIN_DEVICE_KHR}, {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, NULL, hd}};
        uint64_t ts[2], dev;
        PFN_vkGetCalibratedTimestampsKHR f = hvk.dev.x.calibrated_khr ? vkGetCalibratedTimestampsKHR : (PFN_vkGetCalibratedTimestampsKHR)vkGetCalibratedTimestampsEXT;
        if (f && f(hvk.d, 2, ci, ts, &dev) == VK_SUCCESS) {
            gpu = ts[0]; host = ts[1];
#if defined(_WIN32)
            host = plat_now_ns();   // the performance counter's ticks: taken as now (the error is the call's duration)
#endif
            ok = 1;
        }
    }
    uint64_t gn = guest_ns(), pn = plat_now_ns();
    if (!ok) { HVK_RED_ONCE("no calibrated timestamps on this device: GPU timestamps are tied to the guest clock at their first conversion only roughly (offset unknown)"); gpu = 0; host = pn; }
    int64_t off = gn ? (int64_t)gn - (int64_t)pn : 0;   // the guest clock against the monotonic one
    g_cal_g0 = gpu; g_cal_n0 = nvn_timestamp_from_ns((uint64_t)((int64_t)host + off));
}

// ---------- Render passes ----------
struct pass_t {
    int active;
    VkRenderingAttachmentInfo ca[8]; int nc;
    VkRenderingAttachmentInfo da, sa; int has_d, has_s;
    VkFormat cfmt[8], dfmt, sfmt; uint8_t ccls[8]; hvk_view cv[8], dv;
    VkExtent2D extent; uint32_t layers; VkSampleCountFlagBits samples;
    int swrite, occ_open; uint32_t occ_q; int cond_open, xfb_open, xfb_n;
};
static pass_t P;
static int pass_active(void) { return P.active; }
static int g_rc_skip;                      // SetRenderEnable(false): draws and clears are skipped
static int g_cond_on; static VkBuffer g_cond_buf; static VkDeviceSize g_cond_off;   // a GPU condition: the predicate word the passes test
static struct { int valid; VkPipeline pipe; } g_bound;   // the pipeline bound and whether the dynamic state shadow below holds
static struct dyn_shadow { int valid; VkViewport vp; VkRect2D sc; uint32_t cull, front, topo, dt, dw, dcmp, st, sop[2][4], scm[2], swm[2], sref[2], rd, dbe, pr, poly, dclamp, a2c, samples, logic_en, logic, patch;
                           float bias[3], bc[4], lw; VkColorComponentFlags wm[8]; uint32_t ben[8]; VkColorBlendEquationEXT beq[8]; } g_dyn;
static void reports_flush(void);
static void cond_begin(VkCommandBuffer cb) {
    if (!g_cond_on || !hvk.dev.caps.cond_render) return;
    VkConditionalRenderingBeginInfoEXT ci = {VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT, NULL, g_cond_buf, g_cond_off, 0};
    vkCmdBeginConditionalRenderingEXT(cb, &ci); P.cond_open = 1;
}
static VkClearValue clear_value_of(uint8_t cls, const nvn_clear_color *c) {
    VkClearValue v; memset(&v, 0, sizeof v);
    const double ch[4] = {c->red, c->green, c->blue, c->alpha};
    for (int i = 0; i < 4; i++) {   // integer attachments take the value converted, as the Metal backend's clears do
        float f = (float)ch[i];
        if (cls == NVN_VK_UINT) v.color.uint32[i] = f > 0 ? (f < 4294967040.f ? (uint32_t)f : 0xffffffffu) : 0;
        else if (cls == NVN_VK_SINT) v.color.int32[i] = !(f == f) ? 0 : f >= 2147483520.f ? INT32_MAX : f <= -2147483648.f ? INT32_MIN : (int32_t)f;
        else v.color.float32[i] = f;
    }
    return v;
}
static int same_sub(const hvk_view *a, const hvk_view *b) { return a->t == b->t && a->level == b->level && a->layer0 == b->layer0; }
// Begin the render pass of R.rt: the pending full clears become load operations (except under a condition, where they are made inside the pass)
// A depth clear value for format f that stores what a fragment writing depth d stores (rendering.md, "Depth16 load-clears match fragment writes": an EQUAL test after a clear
// must pass for a fragment at the clear depth). Vulkan lets a conversion to unorm take either neighbour of a half-way product, and drivers convert clear values on another path
// than fragments: lavapipe rounds a fragment's float product half to even but converts a clear in double, so 7 of t_vk_depth16_clear's 1042 depths landed one unit apart. For
// DEPTH16 the value is quantized as the fragment path does (the float product, half to even: the hardware conversion, and lavapipe's) and moved a quarter unit inside that
// value's bin, which a clear that truncates or rounds, in float or double, stores as that value. Other formats as they are (D32F has no rounding; D24 is not measured)
float hvk_depth_clear_value(VkFormat f, double d);
float hvk_depth_clear_value(VkFormat f, double d) {
    float z = (float)(d < 0 ? 0 : d > 1 ? 1 : d);
    if ((f != VK_FORMAT_D16_UNORM && f != VK_FORMAT_D16_UNORM_S8_UINT) || !(z > 0) || !(z < 1)) return z;
    float q = rintf(z * 65535.0f);
    return q >= 65535.0f ? 1.0f : (float)(((double)q + 0.25) / 65535.0);
}
static int pass_begin(VkCommandBuffer cb) {
    if (P.active) return 1;
    memset(&P, 0, sizeof P);
    hvk_barrier_if_needed(cb);
    uint32_t w = UINT32_MAX, h = UINT32_MAX, layers = UINT32_MAX; VkSampleCountFlagBits samples = 0;
    int nc = R.has_rt ? R.rt.n : 0; if (nc > 8) nc = 8;
    for (int i = 0; i < nc; i++) {
        P.ca[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        P.ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL; P.ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; P.ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        const tex_t *t = R.rt.color[i];
        hvk_tex *x = t ? hvk_tex_get(t) : NULL;
        if (!x || !hvk_tex_view(x, R.rt.has_cview[i] ? &R.rt.cview[i] : NULL, HVK_VIEW_ATTACH, &P.cv[i])) { if (t) HVK_RED_ONCE("colour target %d (ser=%llu) has no attachment view: not attached", i, (unsigned long long)t->serial); continue; }
        int dup = 0; for (int j = 0; j < i; j++) if (P.ca[j].imageView && same_sub(&P.cv[j], &P.cv[i])) dup = 1;
        if (dup) { HVK_NOTE_ONCE("one texture at two colour slots (first case slot %d): attached once, as the Metal backend's rt_dup_slot", i); P.cv[i].view = VK_NULL_HANDLE; continue; }
        P.ca[i].imageView = P.cv[i].view; P.cfmt[i] = P.cv[i].format; P.ccls[i] = P.cv[i].cls;
        if (R.clear_c[i] && !g_cond_on) { P.ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; P.ca[i].clearValue = clear_value_of(P.cv[i].cls, &R.cc[i]); R.clear_c[i] = 0; }
        if (P.cv[i].w < w) w = P.cv[i].w;
        if (P.cv[i].h < h) h = P.cv[i].h;
        if (P.cv[i].layers < layers) layers = P.cv[i].layers;
        samples = P.cv[i].samples; hvk_tex_gpu_written(x);
    }
    while (nc > 0 && !P.ca[nc - 1].imageView) nc--;   // trailing slots with nothing attached are not the pass's: its pipelines are keyed by the attachments it has (a prewarm row names those)
    P.nc = nc;
    if (R.has_rt && R.rt.depth) {
        hvk_tex *x = hvk_tex_get(R.rt.depth);
        if (x && hvk_tex_view(x, R.rt.has_dview ? &R.rt.dview : NULL, HVK_VIEW_ATTACH, &P.dv)) {
            VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, P.dv.view, VK_IMAGE_LAYOUT_GENERAL, 0, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
                                           VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE};
            P.has_d = (P.dv.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0; P.has_s = (P.dv.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
            P.da = P.sa = a;
            if (P.has_d && R.clear_d && !g_cond_on) { P.da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; P.da.clearValue.depthStencil.depth = hvk_depth_clear_value(P.dv.format, R.cd); R.clear_d = 0; }
            if (P.has_s && R.clear_s && !g_cond_on) { P.sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; P.sa.clearValue.depthStencil.stencil = R.cs & 0xff; R.clear_s = 0; }
            P.dfmt = P.has_d ? P.dv.format : VK_FORMAT_UNDEFINED; P.sfmt = P.has_s ? P.dv.format : VK_FORMAT_UNDEFINED;
            if (P.dv.w < w) w = P.dv.w;
            if (P.dv.h < h) h = P.dv.h;
            if (P.dv.layers < layers) layers = P.dv.layers;
            samples = P.dv.samples; hvk_tex_gpu_written(x);
        } else HVK_RED_ONCE("depth target (ser=%llu) has no attachment view: not attached", (unsigned long long)R.rt.depth->serial);
    }
    if (w == UINT32_MAX) {   // no attachment: the area of the scissor, else 1x1 (a pass of storage writes or of rasterizer discard)
        w = R.has_sc && R.sc.x + R.sc.w > 0 ? (uint32_t)(R.sc.x + R.sc.w) : 1; h = R.has_sc && R.sc.y + R.sc.h > 0 ? (uint32_t)(R.sc.y + R.sc.h) : 1; layers = 1; samples = VK_SAMPLE_COUNT_1_BIT;
    }
    P.extent = (VkExtent2D){w, h}; P.layers = layers ? layers : 1; P.samples = samples ? samples : VK_SAMPLE_COUNT_1_BIT;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, P.extent}, P.layers, 0, (uint32_t)P.nc, P.ca, P.has_d ? &P.da : NULL, P.has_s ? &P.sa : NULL};
    vkCmdBeginRendering(cb, &ri);
    P.active = 1; g_bound.valid = 0; g_dyn.valid = 0;
    atomic_fetch_add(&hvk.st_passes, 1);
    cond_begin(cb);
    if (g_counting && g_cur >= 0 && g_slot[g_cur].nocc < NOCC) {
        P.occ_q = g_slot[g_cur].nocc++; vkCmdBeginQuery(cb, g_slot[g_cur].occ, P.occ_q, VK_QUERY_CONTROL_PRECISE_BIT); P.occ_open = 1;
    } else if (g_counting) HVK_RED_ONCE("more than %d occlusion query segments in one submission: the rest are not counted", NOCC);
    // under a condition the folded clears are made here, so the condition governs them (4.7)
    if (g_cond_on) {
        VkClearAttachment ca[10]; uint32_t n = 0;
        for (int i = 0; i < P.nc; i++) if (R.clear_c[i] && P.ca[i].imageView) { ca[n++] = (VkClearAttachment){VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)i, clear_value_of(P.ccls[i], &R.cc[i])}; R.clear_c[i] = 0; }
        if ((P.has_d && R.clear_d) || (P.has_s && R.clear_s)) {
            VkClearAttachment d = {0}; if (P.has_d && R.clear_d) { d.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT; d.clearValue.depthStencil.depth = hvk_depth_clear_value(P.dfmt, R.cd); }
            if (P.has_s && R.clear_s) { d.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT; d.clearValue.depthStencil.stencil = R.cs & 0xff; }
            ca[n++] = d; R.clear_d = R.clear_s = 0;
        }
        if (n) { VkClearRect rr = {{{0, 0}, P.extent}, 0, P.layers}; vkCmdClearAttachments(cb, n, ca, 1, &rr); }
    }
    return 1;
}
static void xfb_end(VkCommandBuffer cb);
static void pass_end_only(void) {
    if (!P.active) return;
    VkCommandBuffer cb = hvk_main();
    xfb_end(cb);
    if (P.occ_open) { vkCmdEndQuery(cb, g_slot[g_cur].occ, P.occ_q); P.occ_open = 0; }
    if (P.cond_open) { vkCmdEndConditionalRenderingEXT(cb); P.cond_open = 0; }
    vkCmdEndRendering(cb);
    P.active = 0; g_bound.valid = 0; g_need_barrier = 1;
    reports_flush();
}
static int clears_pending(void) { int p = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) p |= R.clear_c[i]; return p && R.has_rt; }
// End the open pass; folded clears with no pass yet land in a pass of their own (before a copy, a dispatch or a new target reads or replaces their targets)
void hvk_pass_end(void) {
    if (g_cur < 0) return;
    if (!P.active && clears_pending()) pass_begin(hvk_main());
    pass_end_only();
    reports_flush();
}
static VkCommandBuffer outside(void) {   // the command buffer, outside any pass, after a barrier when one is needed
    hvk_pass_end();
    VkCommandBuffer cb = hvk_main();
    hvk_barrier_if_needed(cb);
    return cb;
}

// ---------- Submissions ----------
static void slot_reset(int i) {
    slot_t *s = &g_slot[i];
    vkResetCommandPool(hvk.d, s->pool, 0);
    s->pre_begun = 0;
    hvk_ring_slot_release(&s->chunks);
    hvk_pipeline_slot_reset(i);
    if (hvk.dev.x.host_query_reset) { if (s->nocc) vkResetQueryPool(hvk.d, s->occ, 0, NOCC); if (s->nts) vkResetQueryPool(hvk.d, s->ts, 0, NTS); }
    s->nposts = 0; s->fence = 0;
}
void hvk_sub_begin(void);
void hvk_sub_begin(void) {
    if (g_cur >= 0) return;
    static int prewarmed;
    if (!prewarmed) { prewarmed = 1; hvk_prewarm_start(); }   // with the prebuild (DM_VK_PREBUILD=1) this returns once every row is built
    int i = g_next; slot_t *s = &g_slot[i];
    if (s->fence) hvk_wait(s->fence, UINT64_MAX);   // the slot's submission has completed and its CPU work has run
    slot_reset(i);
    g_cur = i; g_next = (i + 1) % NSLOT;
    hvk_ring_bind(&s->chunks);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(s->main, &bi);
    if (!hvk.dev.x.host_query_reset) { VkCommandBuffer pre = hvk_pre(); vkCmdResetQueryPool(pre, s->occ, 0, NOCC); vkCmdResetQueryPool(pre, s->ts, 0, NTS); }
    s->nocc = s->nts = 0; g_occ_first = 0;
    g_open = atomic_load(&hvk_last_fence) + 1;
    g_need_barrier = 1; g_rc_skip = 0; g_cond_on = 0; g_nrep = 0;
    memset(&P, 0, sizeof P); g_bound.valid = 0; g_dyn.valid = 0;
    nvn_gw_reset();
    if (g_counting && !g_acc_valid) {}   // the accumulator keeps its meaning across submissions
}
static void occ_fold(void);
uint64_t hvk_sub_commit(void);
uint64_t hvk_sub_commit(void) {
    if (g_cur < 0) hvk_sub_begin();
    slot_t *s = &g_slot[g_cur];
    hvk_pass_end();
    occ_fold();
    if (s->pre_begun) { hvk_full_barrier(s->pre); vkEndCommandBuffer(s->pre); }
    vkEndCommandBuffer(s->main);
    uint64_t f = g_open;
    VkCommandBufferSubmitInfo cbi[2]; uint32_t n = 0;
    if (s->pre_begun) cbi[n++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, s->pre, 0};
    cbi[n++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, s->main, 0};
    VkSemaphoreSubmitInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, g_timeline, f, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, n, cbi, 1, &sig};
    s->fence = f;
    pthread_mutex_lock(&hvk.queue_mu); VkResult r = vkQueueSubmit2(hvk.dev.queue, 1, &si, VK_NULL_HANDLE); pthread_mutex_unlock(&hvk.queue_mu);
    if (r) {   // a lost device: the fence is signalled from the CPU so waits end (the work is lost, said once)
        HVK_RED_ONCE("vkQueueSubmit2 failed (VkResult %d): the submission's GPU work is lost", r);
        VkSemaphoreSignalInfo ss = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, g_timeline, f}; vkSignalSemaphore(hvk.d, &ss);
    }
    nvn_gw_commit(f, atomic_load(&g_done));
    g_open = 0; g_cur = -1; hvk_ring_bind(NULL);
    pthread_mutex_lock(&g_done_mu); atomic_store(&hvk_last_fence, f); pthread_cond_signal(&g_work_cv); pthread_mutex_unlock(&g_done_mu);
    return f;
}
uint64_t hvk_last(void);
uint64_t hvk_last(void) { return atomic_load(&hvk_last_fence); }
void hvk_finish(void);
void hvk_finish(void) { hvk_wait(atomic_load(&hvk_last_fence), UINT64_MAX); }

int hvk_commands_init(void) {
    VkSemaphoreTypeCreateInfo st = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &st};
    if (vkCreateSemaphore(hvk.d, &sci, NULL, &g_timeline)) return 0;
    for (int i = 0; i < NSLOT; i++) {
        slot_t *s = &g_slot[i];
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, hvk.dev.queue_family};
        if (vkCreateCommandPool(hvk.d, &pi, NULL, &s->pool)) return 0;
        VkCommandBuffer cbs[2];
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, s->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
        if (vkAllocateCommandBuffers(hvk.d, &ai, cbs)) return 0;
        s->pre = cbs[0]; s->main = cbs[1];
        VkQueryPoolCreateInfo oq = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_OCCLUSION, NOCC, 0};
        VkQueryPoolCreateInfo tq = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, NTS, 0};
        if (vkCreateQueryPool(hvk.d, &oq, NULL, &s->occ) || vkCreateQueryPool(hvk.d, &tq, NULL, &s->ts)) return 0;
        if (hvk.dev.x.host_query_reset) { vkResetQueryPool(hvk.d, s->occ, 0, NOCC); vkResetQueryPool(hvk.d, s->ts, 0, NTS); }
    }
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, hvk.dev.queue_family};
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateCommandPool(hvk.d, &pi, NULL, &g_oneoff_pool) || vkCreateFence(hvk.d, &fi, NULL, &g_oneoff_fence)) return 0;
    void *map;
    if (hvk_buffer_make(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, 1, &g_acc, &g_acc_mem, &map)) return 0;
    memset(map, 0, 16);
    if (hvk.dev.caps.buffer_device_address) { VkBufferDeviceAddressInfo ba = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, g_acc}; g_acc_dev = vkGetBufferDeviceAddress(hvk.d, &ba); }
    calibrate();
    g_quit = 0;
    if (pthread_create(&g_thread, NULL, completion, NULL)) return 0;
    g_thread_on = 1;
    return 1;
}
static void static_draw_buffers_free(void);
void hvk_commands_shutdown(void) {
    if (g_cur >= 0) hvk_sub_commit();
    hvk_finish();
    if (g_thread_on) { pthread_mutex_lock(&g_done_mu); g_quit = 1; pthread_cond_signal(&g_work_cv); pthread_mutex_unlock(&g_done_mu); pthread_join(g_thread, NULL); g_thread_on = 0; }
    vkDeviceWaitIdle(hvk.d);
    for (int i = 0; i < NSLOT; i++) {
        slot_t *s = &g_slot[i];
        hvk_ring_slot_release(&s->chunks);
        if (s->occ) vkDestroyQueryPool(hvk.d, s->occ, NULL);
        if (s->ts) vkDestroyQueryPool(hvk.d, s->ts, NULL);
        if (s->pool) vkDestroyCommandPool(hvk.d, s->pool, NULL);
        free(s->posts); memset(s, 0, sizeof *s);
    }
    if (g_oneoff_pool) vkDestroyCommandPool(hvk.d, g_oneoff_pool, NULL);
    if (g_oneoff_fence) vkDestroyFence(hvk.d, g_oneoff_fence, NULL);
    if (g_timeline) vkDestroySemaphore(hvk.d, g_timeline, NULL);
    if (g_acc) { vkDestroyBuffer(hvk.d, g_acc, NULL); vkFreeMemory(hvk.d, g_acc_mem, NULL); hvk.dev.allocations--; }
    static_draw_buffers_free();
    g_oneoff_pool = VK_NULL_HANDLE; g_oneoff_fence = VK_NULL_HANDLE; g_timeline = VK_NULL_HANDLE; g_acc = VK_NULL_HANDLE;
    g_cur = -1; g_next = 0; atomic_store(&g_done, 0); atomic_store(&hvk_last_fence, 0); g_counting = g_acc_valid = 0;
}

// ---------- Counter reports (4.6) ----------
typedef struct { uint32_t src[2], acc[2], ts[2], dst[2]; uint32_t nq, flags, mul[2], g0[2], n0[2]; } report_push;   // the kernel's push constants (shaders/report.comp)
static void addr2(uint32_t o[2], uint64_t a) { o[0] = (uint32_t)a; o[1] = (uint32_t)(a >> 32); }
static void report_run(VkCommandBuffer cb, const report_t *r) {
    slot_t *s = &g_slot[g_cur];
    hvk_alloc a; if (!hvk_ring_alloc((VkDeviceSize)(r->nq + 1) * 8, 16, &a)) return;
    if (r->nq) vkCmdCopyQueryPoolResults(cb, s->occ, r->q0, r->nq, a.buf, a.off, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (r->ts != UINT32_MAX) vkCmdCopyQueryPoolResults(cb, s->ts, r->ts, 1, a.buf, a.off + (VkDeviceSize)r->nq * 8, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    hvk_full_barrier(cb);
    VkBuffer db; VkDeviceSize doff; VkDeviceAddress dd = 0;
    if (r->dst && (!hvk_buf(r->dst, 16, &db, &doff, &dd) || !dd)) { HVK_RED_ONCE("counter report address %#llx is in no memory pool with a device address: not written", (unsigned long long)r->dst); return; }
    report_push p; memset(&p, 0, sizeof p);
    addr2(p.src, a.dev); addr2(p.acc, g_acc_dev); addr2(p.ts, r->ts != UINT32_MAX ? a.dev + (VkDeviceAddress)r->nq * 8 : 0); addr2(p.dst, dd);
    p.nq = r->nq; p.flags = r->flags | (r->ts != UINT32_MAX ? 16u : 0u);
    addr2(p.mul, g_cal_mul); addr2(p.g0, g_cal_g0); addr2(p.n0, g_cal_n0);
    hvk_kernel_dispatch(cb, HVK_K_REPORT, &p, sizeof p, 1);
    g_need_barrier = 1;
}
static void reports_flush(void) {
    if (!g_nrep || P.active || g_cur < 0) return;
    VkCommandBuffer cb = hvk_main();
    hvk_barrier_if_needed(cb);
    for (int i = 0; i < g_nrep; i++) report_run(cb, &g_rep[i]);
    g_nrep = 0; g_bound.valid = 0;
}
static void report_queue(report_t r) {
    if (g_nrep == (int)(sizeof g_rep / sizeof *g_rep)) { int keep = P.active; if (keep) pass_end_only(); else reports_flush(); }
    g_rep[g_nrep++] = r;
    if (!P.active) reports_flush();
}
static uint32_t ts_write(VkCommandBuffer cb, int top) {
    slot_t *s = &g_slot[g_cur];
    if (s->nts >= NTS) { HVK_RED_ONCE("more than %d timestamps in one submission: later reports take none", NTS); return UINT32_MAX; }
    uint32_t q = s->nts++;
    vkCmdWriteTimestamp2(cb, top ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s->ts, q);
    return q;
}
static void occ_fold(void) {   // at the end of a submission: its queries go into the accumulator (the next submission's slot has its own pool)
    if (!g_counting || g_cur < 0) return;
    slot_t *s = &g_slot[g_cur];
    if (g_occ_first >= s->nocc && g_acc_valid) return;
    VkCommandBuffer cb = hvk_main();
    hvk_barrier_if_needed(cb);
    report_t r = {0, g_occ_first, s->nocc - g_occ_first, UINT32_MAX, 1u | (g_acc_valid ? 2u : 0u) | 4u};
    report_run(cb, &r);
    g_occ_first = s->nocc; g_acc_valid = 1;
}
void hvk_report_counter(uint64_t type, uint64_t addr);
void hvk_report_counter(uint64_t type, uint64_t addr) {
    if (g_cur < 0) return;
    nvn_gw_sbw_note(addr, 16, SBW_REPORT);
    VkCommandBuffer cb = hvk_main();
    if (!hvk.dev.caps.buffer_device_address) { HVK_RED_ONCE("counter reports need buffer device addresses: written on the CPU at completion with a zero count"); post_add((post_op){POST_COUNTER, NULL, 0, 0, addr, (uint32_t)type}); return; }
    if (type == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
        if (!g_counting) { g_counting = 1; g_acc_valid = 0; g_occ_first = g_slot[g_cur].nocc; }   // a report before any reset counts from here
        if (P.active && P.occ_open) { vkCmdEndQuery(cb, g_slot[g_cur].occ, P.occ_q); P.occ_open = 0; }
        if (!P.active) hvk_barrier_if_needed(cb);
        uint32_t ts = ts_write(cb, 0);
        slot_t *s = &g_slot[g_cur];
        report_t r = {addr, g_occ_first, s->nocc - g_occ_first, ts, 1u | (g_acc_valid ? 2u : 0u) | 4u};
        g_occ_first = s->nocc; g_acc_valid = 1;
        if (P.active && s->nocc < NOCC) { P.occ_q = s->nocc++; vkCmdBeginQuery(cb, s->occ, P.occ_q, VK_QUERY_CONTROL_PRECISE_BIT); P.occ_open = 1; }
        report_queue(r);
        return;
    }
    if (type == NVN_COUNTER_TYPE_TIMESTAMP || type == NVN_COUNTER_TYPE_TIMESTAMP_TOP) {
        if (!P.active) hvk_barrier_if_needed(cb);
        uint32_t ts = ts_write(cb, type == NVN_COUNTER_TYPE_TIMESTAMP_TOP);
        report_queue((report_t){addr, 0, 0, ts, 8u});   // the counter field holds the timestamp too, as the Metal backend writes it
        return;
    }
    static _Atomic uint32_t seen;
    if (!(atomic_fetch_or(&seen, 1u << (type & 31)) >> (type & 31) & 1))
        hostvk_log("[RED] [hostvk] counter type %llu (pipeline statistics, primitives generated, transform feedback, ZCull): not mapped (phase P8), the report is written on the CPU at completion with a zero count", (unsigned long long)type);
    post_add((post_op){POST_COUNTER, NULL, 0, 0, addr, (uint32_t)type});
}
void hvk_reset_counter(uint64_t type);
void hvk_reset_counter(uint64_t type) {
    if (g_cur < 0 || type != NVN_COUNTER_TYPE_SAMPLES_PASSED) return;
    VkCommandBuffer cb = hvk_main();
    if (P.active && P.occ_open) { vkCmdEndQuery(cb, g_slot[g_cur].occ, P.occ_q); P.occ_open = 0; }
    g_counting = 1; g_acc_valid = 0; g_occ_first = g_slot[g_cur].nocc;   // what was counted before is dropped: the accumulator reads as 0 until the next report stores into it
    if (P.active && g_slot[g_cur].nocc < NOCC) { P.occ_q = g_slot[g_cur].nocc++; vkCmdBeginQuery(cb, g_slot[g_cur].occ, P.occ_q, VK_QUERY_CONTROL_PRECISE_BIT); P.occ_open = 1; }
}

// ---------- Conditional rendering (4.7) ----------
typedef struct { uint32_t a[2], dst[2], mode, pad; } pred_push;
void hvk_render_cond(const p_render_cond *rc);
void hvk_render_cond(const p_render_cond *rc) {
    if (g_cur < 0) return;
    if (rc->mode >= 2) {   // SetRenderEnable: known on the CPU
        if (g_cond_on) { if (P.active && P.cond_open) { vkCmdEndConditionalRenderingEXT(hvk_main()); P.cond_open = 0; } g_cond_on = 0; }
        g_rc_skip = rc->mode == 3;
        return;
    }
    g_rc_skip = 0;
    if (!hvk.dev.caps.cond_render || !hvk.dev.caps.buffer_device_address) {
        HVK_RED_ONCE("conditional rendering without VK_EXT_conditional_rendering: drawn unconditionally");
        g_cond_on = 0; return;
    }
    VkBuffer b; VkDeviceSize off; VkDeviceAddress dev;
    if (!hvk_buf(rc->addr, 32, &b, &off, &dev) || !dev) { HVK_RED_ONCE("conditional rendering on counters at %#llx in no memory pool: drawn unconditionally", (unsigned long long)rc->addr); g_cond_on = 0; return; }
    VkCommandBuffer cb = outside();   // the folded clears before it land unconditionally; the reports before it are written
    if (g_cond_on) g_cond_on = 0;
    hvk_alloc a; if (!hvk_ring_alloc(16, 16, &a)) return;
    pred_push p = {{0}, {0}, (uint32_t)rc->mode, 0}; addr2(p.a, dev); addr2(p.dst, a.dev);
    if (!hvk_kernel_dispatch(cb, HVK_K_PREDICATE, &p, sizeof p, 1)) return;
    g_need_barrier = 1; g_bound.valid = 0;
    g_cond_on = 1; g_cond_buf = a.buf; g_cond_off = a.off;
}

// ---------- Transform feedback ----------
static int sh_xfb(const hvk_shader *s);
static void xfb_begin(VkCommandBuffer cb) {
    if (P.xfb_open || !S.tf_on || !hvk.dev.caps.xfb) return;
    VkBuffer bufs[4], cbuf[4]; VkDeviceSize offs[4], sizes[4], coff[4]; uint32_t n = 0;
    VkBuffer ctlb = VK_NULL_HANDLE; VkDeviceSize ctlo = 0; uint64_t ctl = 0;
    for (int i = 0; i < 4; i++) if (S.tf[i].addr) ctl = 1;
    for (uint32_t i = 0; i < 4; i++) {
        if (!S.tf[i].addr || !hvk_buf(S.tf[i].addr, S.tf[i].size ? S.tf[i].size : 1, &bufs[i], &offs[i], NULL)) break;
        sizes[i] = S.tf[i].size ? S.tf[i].size : VK_WHOLE_SIZE; n = i + 1;
    }
    if (!n) { HVK_RED_ONCE("transform feedback on with no capture buffer bound: nothing captured"); return; }
    extern uint64_t hvk_tf_ctl;
    ctl = hvk_tf_ctl;
    if (!ctl || !hvk_buf(ctl, 16, &ctlb, &ctlo, NULL)) { HVK_RED_ONCE("transform feedback control buffer %#llx in no memory pool: captured from offset 0 and not saved", (unsigned long long)ctl); ctlb = VK_NULL_HANDLE; }
    vkCmdBindTransformFeedbackBuffersEXT(cb, 0, n, bufs, offs, sizes);
    for (uint32_t i = 0; i < n; i++) { cbuf[i] = ctlb; coff[i] = ctlo + 4 * i; }
    vkCmdBeginTransformFeedbackEXT(cb, 0, ctlb ? n : 0, ctlb ? cbuf : NULL, ctlb ? coff : NULL);
    P.xfb_open = 1; P.xfb_n = (int)n;
}
static void xfb_end(VkCommandBuffer cb) {
    if (!P.xfb_open) return;
    extern uint64_t hvk_tf_ctl;
    VkBuffer ctlb = VK_NULL_HANDLE, cbuf[4]; VkDeviceSize ctlo = 0, coff[4];
    if (hvk_tf_ctl && !hvk_buf(hvk_tf_ctl, 16, &ctlb, &ctlo, NULL)) ctlb = VK_NULL_HANDLE;
    for (int i = 0; i < P.xfb_n; i++) { cbuf[i] = ctlb; coff[i] = ctlo + 4 * (VkDeviceSize)i; }
    vkCmdEndTransformFeedbackEXT(cb, 0, ctlb ? (uint32_t)P.xfb_n : 0, ctlb ? cbuf : NULL, ctlb ? coff : NULL);
    if (hvk_tf_ctl) nvn_gw_sbw_note(hvk_tf_ctl, 16, SBW_GPU);
    P.xfb_open = 0;
}
uint64_t hvk_tf_ctl;   // the control buffer of the capture in progress (its four byte offsets: the counters, nvn_feedback.h)
void hvk_tf_state(const p_tf_state *b);
void hvk_tf_state(const p_tf_state *b) {
    if (g_cur < 0) return;
    if (!hvk.dev.caps.xfb) { HVK_RED_ONCE("transform feedback without VK_EXT_transform_feedback: not captured"); return; }
    VkCommandBuffer cb = outside();   // a capture begins, pauses, resumes or ends between passes: the pass's end writes the counters into the control buffer
    if (b->what == 0 || b->what == 3) {
        hvk_tf_ctl = b->ctl;
        if (b->what == 0 && b->ctl) { VkBuffer cbuf; VkDeviceSize co; if (hvk_buf(b->ctl, 16, &cbuf, &co, NULL)) { vkCmdFillBuffer(cb, cbuf, co & ~(VkDeviceSize)3, 16, 0); g_need_barrier = 1; } }
    }
}

// ---------- Draws ----------
static VkPrimitiveTopology topology(int prim, int *klass, int *convert) {
    *convert = 0;
    switch (prim) {
        case NVN_DRAW_PRIMITIVE_POINTS: *klass = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case NVN_DRAW_PRIMITIVE_LINES: *klass = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case NVN_DRAW_PRIMITIVE_LINE_LOOP: *klass = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *convert = 1; return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case NVN_DRAW_PRIMITIVE_LINE_STRIP: *klass = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case NVN_DRAW_PRIMITIVE_TRIANGLES: *klass = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: case NVN_DRAW_PRIMITIVE_QUAD_STRIP: *klass = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: case NVN_DRAW_PRIMITIVE_POLYGON: *klass = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case NVN_DRAW_PRIMITIVE_QUADS: *klass = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *convert = 2; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: *klass = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
        case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: *klass = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
        case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: *klass = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: *klass = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
        case NVN_DRAW_PRIMITIVE_PATCHES: *klass = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST; return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        default: *klass = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}
static int is_strip(VkPrimitiveTopology t) {
    return t == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
           t == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY || t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
}
static VkStencilOp sop(uint32_t o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }
static VkCompareOp cop(uint32_t f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }
static VkBuffer g_zero_vb; static VkDeviceMemory g_zero_vb_mem;
static VkBuffer g_quad_ib; static VkDeviceMemory g_quad_ib_mem; enum { QUAD_N = 65536 };
static int static_draw_buffers(void) {
    if (g_quad_ib) return 1;
    void *map;
    if (!g_zero_vb) {
        if (hvk_buffer_make(256, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1, &g_zero_vb, &g_zero_vb_mem, &map)) return 0;
        memset(map, 0, 256);
    }
    if (hvk_buffer_make((VkDeviceSize)QUAD_N * 24, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 1, &g_quad_ib, &g_quad_ib_mem, &map)) return 0;
    uint32_t *ix = map; static const uint32_t pat[6] = {0, 1, 2, 0, 2, 3};
    for (uint32_t q = 0; q < QUAD_N; q++) for (int k = 0; k < 6; k++) ix[q * 6 + (uint32_t)k] = 4 * q + pat[k];
    return 1;
}
static void static_draw_buffers_free(void) {
    hvk_buffer_free(g_zero_vb, g_zero_vb_mem); hvk_buffer_free(g_quad_ib, g_quad_ib_mem);
    g_zero_vb = g_quad_ib = VK_NULL_HANDLE; g_zero_vb_mem = g_quad_ib_mem = VK_NULL_HANDLE;
}
// Indices converted on the CPU into the ring: quads and fans into triangles (nvn_primitive.h, restart-aware), restart with an index Vulkan cannot (other than all ones),
// lists with restart (Vulkan restarts strips and fans only), 8-bit indices on a device without them. 0 when the source cannot be read on the CPU now (written by the GPU in this submission)
static int cpu_readable(uint64_t a, uint64_t n);
// Indexed quads whose indices the CPU cannot read now (cpu_readable): the index helper (shaders/quad_index.comp) writes the triangle list into the ring on the GPU, outside the pass.
// 1 done, 0 failed, -1 not possible here (no device address for the source)
static int quads_gpu(const p_draw_idx *di, hvk_alloc *out, uint32_t *count) {
    int isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4; uint32_t quads = (uint32_t)di->count / 4;
    VkBuffer b; VkDeviceSize o; VkDeviceAddress src = 0;
    if (!hvk.dev.caps.buffer_device_address || !hvk_buf(di->ibuf & ~3ull, (uint64_t)di->count * (uint64_t)isz + 4, &b, &o, &src) || !src) return -1;
    *count = quads * 6;
    if (!hvk_ring_alloc((VkDeviceSize)(quads ? quads : 1) * 24, 16, out) || !out->dev) return 0;
    if (!quads) return 1;
    struct { uint32_t src[2], dst[2], quads, isz, shift, pad; } p = {{(uint32_t)src, (uint32_t)(src >> 32)}, {(uint32_t)out->dev, (uint32_t)(out->dev >> 32)}, quads, (uint32_t)isz, (uint32_t)(di->ibuf & 3), 0};
    VkCommandBuffer cb = outside();
    hvk_full_barrier(cb);   // after the writes the kernel reads
    if (!hvk_kernel_dispatch(cb, HVK_K_QUAD, &p, sizeof p, (quads + 63) / 64)) return 0;
    g_need_barrier = 1;     // the draw's index fetch after the kernel's writes
    g_bound.valid = 0;
    atomic_fetch_add(&hvk.st_index_gpu, 1);
    return 1;
}
static int convert_indices(const p_draw_idx *di, int prim, VkPrimitiveTopology *topo, hvk_alloc *out, uint32_t *count) {
    int isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4; uint64_t n = (uint64_t)di->count * (uint64_t)isz;
    if (!cpu_readable(di->ibuf, n)) {
        if (prim == NVN_DRAW_PRIMITIVE_QUADS && !S.restart.on) {   // the helper takes fixed groups of four: a restart index would have to split them
            int r = quads_gpu(di, out, count);
            if (r >= 0) { *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return r; }
        }
        HVK_RED_ONCE("indices the GPU writes before this draw need a conversion the GPU index helper does not do (primitive %d, restart %d, %d-byte indices): converted from their current content on the CPU", prim, S.restart.on, isz);
    }
    const uint8_t *src = (const uint8_t *)(uintptr_t)di->ibuf;
    uint32_t cap = (uint32_t)di->count * 3 + 6;
    if (!hvk_ring_alloc((VkDeviceSize)cap * 4, 16, out)) return 0;
    uint32_t *d = (uint32_t *)(void *)out->cpu, k = 0, all = isz == 1 ? 0xffu : isz == 2 ? 0xffffu : 0xffffffffu;
    if (prim == NVN_DRAW_PRIMITIVE_QUADS || (prim == NVN_DRAW_PRIMITIVE_TRIANGLE_FAN && S.restart.on && S.restart.index != all)) {
        k = nvn_expand_primitive(d, src, (uint32_t)di->count, (unsigned)isz, (unsigned)prim, S.restart.on, S.restart.index);
        *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    } else {
        int group = *topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST ? 1 : *topo == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ? 2 : *topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ? 3 :
                    *topo == VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY ? 4 : *topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY ? 6 : 0;
        uint32_t have = 0, start = 0;
        for (uint32_t i = 0; i < (uint32_t)di->count; i++) {
            uint32_t ix = isz == 1 ? src[i] : 0; if (isz == 2) { uint16_t v; memcpy(&v, src + 2 * (size_t)i, 2); ix = v; } else if (isz == 4) memcpy(&ix, src + 4 * (size_t)i, 4);
            int rs = S.restart.on && ix == S.restart.index;
            if (group) {   // a list: a restart drops the primitive it cuts short
                if (rs) { k = start; have = 0; continue; }
                d[k++] = ix; if (++have == (uint32_t)group) { have = 0; start = k; }
            } else d[k++] = rs ? 0xffffffffu : ix;   // a strip: the restart Vulkan knows
        }
        if (group) k = start;
    }
    *count = k;
    return 1;
}
// Whether the CPU can read guest memory [a, a + n) now, at encode time: no GPU write queued in this submission covers it (copies, updates, fills, storage writes) and none of an
// earlier submission still running (nvn_gpuwrite.h). Otherwise what a draw needs from it is copied or converted on the GPU, in order, before the draw's pass
static int cpu_readable(uint64_t a, uint64_t n) {
    return !nvn_gw_pend_hits(a, n) && !nvn_gw_sbw_hits(a, a + n) && !nvn_gw_live(a, a + n, hvk_done_fence());
}
// Unaligned streams the CPU cannot read now (cpu_readable): copied into the ring on the GPU, outside the pass, before the draw's pass begins; bind_vertex_buffers binds the copies
static struct { VkBuffer buf; VkDeviceSize off; } g_vcopy[16]; static uint32_t g_vcopy_mask;
static int vertex_copies_gpu(const hvk_vin *v) {
    g_vcopy_mask = 0;
    for (uint32_t i = 0; i < v->nb; i++) {
        uint32_t s = v->b[i].binding;
        if (s >= 16) continue;
        const NVNbufferRange *r = &S.vbuf[s];
        uint64_t len = r->size ? r->size : 1;
        if (!(r->address & 3) || cpu_readable(r->address, len)) continue;
        VkBuffer b; VkDeviceSize o; hvk_alloc a;
        if (!hvk_buf(r->address, len, &b, &o, NULL)) continue;   // in no pool: bind_vertex_buffers says so
        if (!hvk_ring_alloc(len, 16, &a)) return 0;
        VkCommandBuffer cb = outside();
        hvk_full_barrier(cb);   // after the writes (of this submission or a running one) the copy reads
        VkBufferCopy c = {o, a.off, len};
        vkCmdCopyBuffer(cb, b, a.buf, 1, &c);
        g_need_barrier = 1;
        g_vcopy[s].buf = a.buf; g_vcopy[s].off = a.off; g_vcopy_mask |= 1u << s;
        atomic_fetch_add(&hvk.st_vcopies_gpu, 1);
    }
    return 1;
}
// Vertex buffer bindings: a stream whose address the format cannot fetch aligned is copied into the ring (on the CPU, or on the GPU when vertex_copies_gpu did)
static int bind_vertex_buffers(VkCommandBuffer cb, const hvk_vin *v) {
    VkBuffer bufs[17]; VkDeviceSize offs[17], sizes[17], strides[17]; uint32_t first = 0, n = 0;
    for (uint32_t i = 0; i < v->nb; i++) {
        uint32_t s = v->b[i].binding;
        VkBuffer b; VkDeviceSize o, sz;
        if (s == 16) { b = g_zero_vb; o = 0; sz = 256; }
        else {
            const NVNbufferRange *r = &S.vbuf[s];
            uint64_t len = r->size ? r->size : 1;
            int aligned = (r->address & 3) == 0;
            if (!aligned && (g_vcopy_mask >> s & 1)) { b = g_vcopy[s].buf; o = g_vcopy[s].off; }   // copied on the GPU before the pass
            else if (!aligned) {   // byte-aligned streams (Arkham Knight's at 2 mod 4): Vulkan fetches components at their own alignment, so the bytes go to an aligned copy
                hvk_alloc a;
                if (!hvk_ring_alloc(len, 16, &a)) return 0;
                memcpy(a.cpu, (const void *)(uintptr_t)r->address, len); b = a.buf; o = a.off;
            } else if (!hvk_buf(r->address, len, &b, &o, NULL)) { HVK_RED_ONCE("vertex buffer %u at %#llx is in no memory pool: the zero buffer is read", s, (unsigned long long)r->address); b = g_zero_vb; o = 0; len = 256; }
            sz = len;
        }
        if (n && s != first + n) { vkCmdBindVertexBuffers2(cb, first, n, bufs, offs, sizes, hvk.dev.caps.vertex_input_dynamic ? NULL : strides); n = 0; }
        if (!n) first = s;
        bufs[n] = b; offs[n] = o; sizes[n] = sz; strides[n] = v->b[i].stride; n++;
    }
    if (n) vkCmdBindVertexBuffers2(cb, first, n, bufs, offs, sizes, hvk.dev.caps.vertex_input_dynamic ? NULL : strides);
    return 1;
}
static int g_mdi_skip_n; static uint64_t g_mdi_cnt;   // the records of an indirect-count multi-draw already drawn by its first one
static void dyn_apply(VkCommandBuffer cb, VkPrimitiveTopology topo, int strip, int restart_native, const hvk_gfx_key *k) {
    struct dyn_shadow n; memset(&n, 0, sizeof n);
    uint32_t tw = P.extent.width, th = P.extent.height;
    VkViewport vp = S.has_vp ? (VkViewport){S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3], S.dr[0], S.dr[1]} : (VkViewport){0, 0, (float)tw, (float)th, S.dr[0], S.dr[1]};
    if (!S.has_vp) { vp.minDepth = 0; vp.maxDepth = 1; }
    uint32_t sw; memcpy(&sw, S.vp_sw[0], 4);
    int flip = !atomic_load(&hvk.origin_ll);   // upper-left: NDC +y to the first row (Vulkan's default puts it on the last, as GL's lower-left does)
    if (sw == 0x100) flip ^= 1;                // NEGATIVE_Y: clip y negated, the viewport flipped about its centre
    else if (sw) HVK_RED_ONCE("viewport swizzle %d,%d,%d,%d not implemented (drawn unswizzled)", S.vp_sw[0][0], S.vp_sw[0][1] ^ 2, S.vp_sw[0][2] ^ 4, S.vp_sw[0][3] ^ 6);
    if (flip) { vp.y += vp.height; vp.height = -vp.height; }
    if (vp.minDepth < 0) vp.minDepth = 0;
    if (vp.minDepth > 1) vp.minDepth = 1;
    if (vp.maxDepth < 0) vp.maxDepth = 0;
    if (vp.maxDepth > 1) vp.maxDepth = 1;
    if (vp.width == 0) vp.width = 1;
    VkRect2D sc = {{0, 0}, {tw, th}};
    if (R.has_sc && !S.rast_discard) {
        long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
        if (x1 > (long)tw) x1 = (long)tw;
        if (y1 > (long)th) y1 = (long)th;
        sc = x1 > x0 && y1 > y0 ? (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}} : (VkRect2D){{0, 0}, {0, 0}};
    }
    n.vp = vp; n.sc = sc;
    n.cull = S.poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT : S.poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT : S.poly.cull == NVN_FACE_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
    // NVN's raster flip is captured when the queue is initialized (rendering.md, "Queue initialization captures the raster origin"): the winding follows the queue's origin
    n.front = (S.poly.front == NVN_FRONT_FACE_CCW) != (hvk.queue_origin_ll != 0) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    n.topo = topo;
    int hd = P.has_d, hs = P.has_s;
    n.dt = hd && S.ds.dt; n.dw = hd && S.ds.dw; n.dcmp = cop(S.ds.df);
    n.st = hs && S.ds.st;
    n.sop[0][0] = sop(S.ds.fail_f); n.sop[0][1] = sop(S.ds.zpass_f); n.sop[0][2] = sop(S.ds.zfail_f); n.sop[0][3] = cop(S.ds.sf_f);
    n.sop[1][0] = sop(S.ds.fail_b); n.sop[1][1] = sop(S.ds.zpass_b); n.sop[1][2] = sop(S.ds.zfail_b); n.sop[1][3] = cop(S.ds.sf_b);
    for (int f = 0; f < 2; f++) { n.scm[f] = S.st_vmask[f] & 0xff; n.swm[f] = S.st_wmask[f] & 0xff; n.sref[f] = S.st_ref[f] & 0xff; }
    n.rd = S.rast_discard != 0;
    uint32_t pm = S.poly.mode == NVN_POLYGON_MODE_POINT ? VK_POLYGON_MODE_POINT : S.poly.mode == NVN_POLYGON_MODE_LINE ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    n.poly = pm;
    unsigned oe = S.poly.offset_en;
    n.dbe = (pm == VK_POLYGON_MODE_FILL && (oe & NVN_POLYGON_OFFSET_ENABLE_FILL)) || (pm == VK_POLYGON_MODE_LINE && (oe & NVN_POLYGON_OFFSET_ENABLE_LINE)) || (pm == VK_POLYGON_MODE_POINT && (oe & NVN_POLYGON_OFFSET_ENABLE_POINT));
    n.bias[0] = n.dbe ? S.po.units : 0; n.bias[1] = n.dbe ? S.po.factor : 0; n.bias[2] = n.dbe ? S.po.clamp : 0;
    n.pr = strip && restart_native && S.restart.on;
    memcpy(n.bc, S.blend_color, 16); n.lw = 1;
    n.dclamp = S.depth_clamp != 0; n.a2c = S.ms.a2c != 0; n.samples = P.samples; n.logic_en = S.color.logic_op != NVN_LOGIC_OP_COPY; n.logic = S.color.logic_op;
    for (int i = 0; i < P.nc; i++) hvk_blend_of(&S.blend[i], (int)(S.color.blend_en >> i & 1), P.cfmt[i], S.chmask.mask >> (4 * i), &n.ben[i], &n.beq[i], &n.wm[i]);
    int all = !g_dyn.valid;
    #define CH(field) (all || memcmp(&n.field, &g_dyn.field, sizeof n.field))
    if (CH(vp)) vkCmdSetViewportWithCount(cb, 1, &n.vp);
    if (CH(sc)) vkCmdSetScissorWithCount(cb, 1, &n.sc);
    if (CH(lw)) vkCmdSetLineWidth(cb, n.lw);
    if (CH(bias)) vkCmdSetDepthBias(cb, n.bias[0], n.bias[2], n.bias[1]);
    if (CH(bc)) vkCmdSetBlendConstants(cb, n.bc);
    if (CH(scm)) { vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, n.scm[0]); vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, n.scm[1]); }
    if (CH(swm)) { vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, n.swm[0]); vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, n.swm[1]); }
    if (CH(sref)) { vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, n.sref[0]); vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, n.sref[1]); }
    if (CH(cull)) vkCmdSetCullMode(cb, n.cull);
    if (CH(front)) vkCmdSetFrontFace(cb, (VkFrontFace)n.front);
    if (CH(topo)) vkCmdSetPrimitiveTopology(cb, (VkPrimitiveTopology)n.topo);
    if (CH(dt)) vkCmdSetDepthTestEnable(cb, n.dt);
    if (CH(dw)) vkCmdSetDepthWriteEnable(cb, n.dw);
    if (CH(dcmp)) vkCmdSetDepthCompareOp(cb, (VkCompareOp)n.dcmp);
    if (all) { vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE); vkCmdSetDepthBounds(cb, 0, 1); }
    if (CH(st)) vkCmdSetStencilTestEnable(cb, n.st);
    if (CH(sop)) { vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, (VkStencilOp)n.sop[0][0], (VkStencilOp)n.sop[0][1], (VkStencilOp)n.sop[0][2], (VkCompareOp)n.sop[0][3]);
                   vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, (VkStencilOp)n.sop[1][0], (VkStencilOp)n.sop[1][1], (VkStencilOp)n.sop[1][2], (VkCompareOp)n.sop[1][3]); }
    if (CH(rd)) vkCmdSetRasterizerDiscardEnable(cb, n.rd);
    if (CH(dbe)) vkCmdSetDepthBiasEnable(cb, n.dbe);
    if (CH(pr)) vkCmdSetPrimitiveRestartEnable(cb, n.pr);
    uint32_t e = hvk.dev.eds_present;
    if ((e >> HOSTVK_EDS_LOGIC_OP & 1) && CH(logic)) vkCmdSetLogicOpEXT(cb, (VkLogicOp)n.logic);
    n.patch = k && k->patch_points ? k->patch_points : 1;   // a pipeline with the patch size dynamic needs one set before any draw, patches or not
    if ((e >> HOSTVK_EDS_PATCH_POINTS & 1) && CH(patch)) vkCmdSetPatchControlPointsEXT(cb, n.patch);
    if ((e >> HOSTVK_EDS_LOGIC_OP_ENABLE & 1) && CH(logic_en)) vkCmdSetLogicOpEnableEXT(cb, n.logic_en);
    if ((e >> HOSTVK_EDS_DEPTH_CLAMP & 1) && CH(dclamp)) vkCmdSetDepthClampEnableEXT(cb, n.dclamp);
    if ((e >> HOSTVK_EDS_POLYGON_MODE & 1) && CH(poly)) vkCmdSetPolygonModeEXT(cb, (VkPolygonMode)n.poly);
    if ((e >> HOSTVK_EDS_SAMPLES & 1) && CH(samples)) vkCmdSetRasterizationSamplesEXT(cb, (VkSampleCountFlagBits)n.samples);
    if ((e >> HOSTVK_EDS_SAMPLE_MASK & 1) && (all || CH(samples))) { VkSampleMask m = 0xffffffffu; vkCmdSetSampleMaskEXT(cb, (VkSampleCountFlagBits)n.samples, &m); }
    if ((e >> HOSTVK_EDS_ALPHA_TO_COVERAGE & 1) && CH(a2c)) vkCmdSetAlphaToCoverageEnableEXT(cb, n.a2c);
    if (P.nc) {
        if ((e >> HOSTVK_EDS_BLEND_ENABLE & 1) && CH(ben)) vkCmdSetColorBlendEnableEXT(cb, 0, (uint32_t)P.nc, n.ben);
        if ((e >> HOSTVK_EDS_BLEND_EQUATION & 1) && CH(beq)) vkCmdSetColorBlendEquationEXT(cb, 0, (uint32_t)P.nc, n.beq);
        if ((e >> HOSTVK_EDS_WRITE_MASK & 1) && CH(wm)) vkCmdSetColorWriteMaskEXT(cb, 0, (uint32_t)P.nc, n.wm);
    }
    #undef CH
    n.valid = 1; g_dyn = n;
}
// The draw's key source (vk_pso.h): the pass's attachments and S's state, which hvk_key_fill reduces to what the device cannot set dynamically (design 3.1)
static void key_src(hvk_key_src *src, const hvk_vin *v) {
    memset(src, 0, sizeof *src);
    src->nc = P.nc; memcpy(src->cfmt, P.cfmt, sizeof src->cfmt); memcpy(src->ccls, P.ccls, sizeof src->ccls); src->dfmt = P.dfmt; src->sfmt = P.sfmt; src->samples = P.samples;
    src->blend = S.blend; src->blend_en = S.color.blend_en; src->chmask = S.chmask.mask; src->logic_op = S.color.logic_op; src->poly_mode = S.poly.mode;
    src->depth_clamp = S.depth_clamp; src->a2c = S.ms.a2c; src->vin = v;
}
static uint32_t bound_streams(void) { uint32_t b = 0; for (int s = 0; s < 16; s++) if (S.vbuf[s].address) b |= 1u << s; return b; }
// A pipeline no list named was built for this draw: its P row for the device's user list (design 3.3), in NVN terms (the formats the pass attaches, a view's where it sets one)
static hvk_shader *g_miss_sh[6];
static void user_row(void) {
    uint16_t cf[8] = {0}, df = 0, sf = 0;
    for (int i = 0; i < P.nc && i < 8; i++) if (P.ca[i].imageView && R.rt.color[i]) cf[i] = (uint16_t)(R.rt.has_cview[i] && R.rt.cview[i].set_format ? R.rt.cview[i].format : R.rt.color[i]->d.format);
    if (R.rt.depth) { uint16_t f = (uint16_t)(R.rt.has_dview && R.rt.dview.set_format ? R.rt.dview.format : R.rt.depth->d.format); if (P.has_d) df = f; if (P.has_s) sf = f; }
    const hvk_shader *vs = g_miss_sh[0], *fs = g_miss_sh[1];
    if (!vs || g_miss_sh[2] || g_miss_sh[3] || g_miss_sh[4]) return;   // a row names a vertex and a fragment shader only (nvn_pipe.h's TODO list: geometry and tessellation)
    uint32_t inputs = 0; for (int a = 0; a < 16; a++) if (vs->vin_types[a]) inputs |= 1u << a;
    char vk[24] = {0}, fk[24] = {0}; memcpy(vk, vs->key, 16); if (fs) memcpy(fk, fs->key, 16);
    nvn_pipe_desc d;
    if (nvn_pipe_from_state(&d, &S, vk, 0, inputs, fs ? fk : NULL, 1, cf, df, sf, (uint32_t)P.samples) == 0) hvk_prewarm_user_row(&d);
}
static int sh_xfb(const hvk_shader *s) { return s && s->xfb; }   // the module declares the Xfb execution mode: it captures
void hvk_draw(const p_draw *dw, const p_draw_idx *di, const uint8_t *rec, uint32_t size);
void hvk_draw(const p_draw *dw, const p_draw_idx *di, const uint8_t *rec, uint32_t size) {
    if (g_cur < 0 || g_rc_skip) return;
    uint64_t mdi_cnt = dw ? dw->mdi_cnt : di->mdi_cnt; int mdi_i = dw ? dw->mdi_i : di->mdi_i;
    if (mdi_cnt && mdi_i > 0 && mdi_cnt == g_mdi_cnt && mdi_i < g_mdi_skip_n) return;   // drawn by the group's first record
    if (!S.prog) { HVK_RED_ONCE("no program bound before the draw: skipped"); return; }
    hvk_prog *pr = hvk_prog_get(S.prog);
    if (!pr || !pr->sh[0]) { HVK_RED_ONCE("draw with no vertex shader (program ser=%llu): skipped", (unsigned long long)S.prog->serial); return; }
    hvk_shader *sh[6] = {pr->sh[0], S.rast_discard ? pr->sh[1] : pr->sh[1], pr->sh[2], pr->sh[3], pr->sh[4], NULL};
    if (!static_draw_buffers()) return;
    if (S.color.alpha_func && S.color.alpha_func != NVN_ALPHA_FUNC_ALWAYS) HVK_RED_ONCE("alpha test function %u: not implemented (the translator's support buffer has it off)", (unsigned)S.color.alpha_func);
    int prim = dw ? dw->prim : di->prim, klass, conv;
    VkPrimitiveTopology topo = topology(prim, &klass, &conv);
    uint32_t inst = (uint32_t)(dw ? (dw->instances > 0 ? dw->instances : 1) : (di->instances > 0 ? di->instances : 1));
    uint64_t ind = dw ? dw->ind : di->ind;
    // Indices: native, or converted on the CPU before the pass begins (the ring is host memory)
    VkBuffer ib = VK_NULL_HANDLE; VkDeviceSize ioff = 0; VkIndexType itype = VK_INDEX_TYPE_UINT32; uint32_t icount = 0; int restart_native = 1;
    if (di) {
        int isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4; uint32_t all = isz == 1 ? 0xffu : isz == 2 ? 0xffffu : 0xffffffffu;
        int need_conv = conv == 2 || (S.restart.on && ((is_strip(topo) && S.restart.index != all) || !is_strip(topo))) || (isz == 1 && !hvk.dev.x.index_uint8) || conv == 1;
        if (ind && need_conv) { HVK_RED_ONCE("indirect indexed draw of primitive %d with restart %d / 8-bit indices: needs the GPU index helper (phase P4), skipped", prim, S.restart.on); return; }
        if (need_conv && !ind) {
            hvk_alloc a;
            if (conv == 1) { HVK_RED_ONCE("indexed line loops: not converted (drawn as line strips, the closing line missing)"); }
            if (!convert_indices(di, prim, &topo, &a, &icount)) return;
            ib = a.buf; ioff = a.off; itype = VK_INDEX_TYPE_UINT32;
        } else {
            if (!hvk_buf(di->ibuf, 1, &ib, &ioff, NULL)) { HVK_RED_ONCE("index buffer %#llx is in no memory pool: the draw is skipped", (unsigned long long)di->ibuf); return; }
            itype = isz == 1 ? VK_INDEX_TYPE_UINT8 : isz == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32; icount = (uint32_t)di->count;
            if (ioff % (VkDeviceSize)isz) { HVK_RED_ONCE("index buffer at an address not aligned to its index size: skipped"); return; }
        }
        restart_native = !need_conv || is_strip(topo);
    } else if (conv == 2) {
        if (ind) { HVK_RED_ONCE("indirect quads: need the GPU index helper (phase P4), skipped"); return; }
        if (dw->count / 4 > QUAD_N) HVK_RED_ONCE("a quad draw of %d vertices: only the first %d quads are drawn", dw->count, QUAD_N);
        topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    } else if (conv == 1 && !ind) HVK_RED_ONCE("line loops: drawn as line strips (the closing line missing)");
    hvk_vin v; hvk_vertex_input(sh[0], S.attr, S.stream, bound_streams(), &v);
    if (!vertex_copies_gpu(&v)) return;   // before the pass: they end the one open
    VkCommandBuffer cb = hvk_main();
    if (!pass_begin(cb)) return;
    hvk_key_src src; key_src(&src, &v);
    hvk_gfx_key k; hvk_key_fill(&k, sh, klass, &src);
    hvk_layout *l = hvk_layout_for(sh);
    if (!l) return;
    memcpy(g_miss_sh, sh, sizeof g_miss_sh);
    VkPipeline pipe = hvk_gfx_pipeline(sh, l, &k, user_row);   // a pipeline not ready yet is made or waited for here: the wait is counted (vk_gpl.c)
    if (!pipe) return;
    if (!g_bound.valid || g_bound.pipe != pipe) {
        if (P.xfb_open && !sh_xfb(sh[2] ? sh[2] : sh[4] ? sh[4] : sh[0])) xfb_end(cb);   // the last stage before rasterization must capture while the capture is begun
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); g_bound.pipe = pipe; g_bound.valid = 1;
    }
    dyn_apply(cb, topo, is_strip(topo), restart_native, &k);
    if (hvk.dev.caps.vertex_input_dynamic) vkCmdSetVertexInputEXT(cb, v.nb, v.b, v.na, v.a);
    if (!bind_vertex_buffers(cb, &v)) return;
    if (!hvk_bind_resources(cb, l, sh, VK_PIPELINE_BIND_POINT_GRAPHICS, (uint32_t)(dw ? dw->base_instance : di->base_instance), NULL)) return;
    if (pr->sh[0]->writes_storage || (pr->sh[1] && pr->sh[1]->writes_storage)) P.swrite = 1;
    if (S.tf_on && sh_xfb(sh[2] ? sh[2] : sh[4] ? sh[4] : sh[0])) xfb_begin(cb);
    atomic_fetch_add(&hvk.st_draws, 1);
    if (dw && !ind) {
        if (conv == 2) { if (dw->count >= 4) { vkCmdBindIndexBuffer(cb, g_quad_ib, 0, VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cb, (uint32_t)(dw->count / 4 > QUAD_N ? QUAD_N : dw->count / 4) * 6, inst, 0, dw->first, (uint32_t)dw->base_instance); } }
        else if (dw->count > 0) vkCmdDraw(cb, (uint32_t)dw->count, inst, (uint32_t)dw->first, (uint32_t)dw->base_instance);
        return;
    }
    if (di) vkCmdBindIndexBuffer(cb, ib, ioff, itype);
    if (!ind) { if (icount) vkCmdDrawIndexed(cb, icount, inst, 0, di->base_vertex, (uint32_t)di->base_instance); return; }
    VkBuffer ab; VkDeviceSize aoff;
    if (!hvk_buf(ind, dw ? 16 : 20, &ab, &aoff, NULL) || (aoff & 3)) { HVK_RED_ONCE("indirect draw arguments at %#llx in no memory pool (or not 4-byte aligned): skipped", (unsigned long long)ind); return; }
    if (mdi_cnt) {   // an indirect-count multi-draw (4.8): its records are consecutive, drawn as one vkCmdDraw*IndirectCount from the first
        VkBuffer cbuf; VkDeviceSize coff;
        if (!hvk_buf(mdi_cnt, 4, &cbuf, &coff, NULL) || (coff & 3)) { HVK_RED_ONCE("multi-draw count at %#llx in no memory pool: skipped", (unsigned long long)mdi_cnt); return; }
        uint32_t maxn = 1; uint64_t stride = dw ? 16 : 20;
        const uint8_t *q = rec + size;
        for (;;) {   // the group's other records follow this one (hle_nvn.c records max of them in a row)
            op_hdr h; memcpy(&h, q, sizeof h);
            if (h.op != (dw ? OP_DRAW : OP_DRAW_IDX)) break;
            uint64_t c2; int i2; uint64_t ind2;
            if (dw) { p_draw t; const p_draw *x = draw_rec(q + 8, h.size, &t); c2 = x->mdi_cnt; i2 = x->mdi_i; ind2 = x->ind; }
            else { p_draw_idx t; const p_draw_idx *x = draw_idx_rec(q + 8, h.size, &t); c2 = x->mdi_cnt; i2 = x->mdi_i; ind2 = x->ind; }
            if (c2 != mdi_cnt || i2 != (int)maxn) break;
            if (maxn == 1) stride = ind2 - ind;
            maxn++; q += h.size;
        }
        if (stride < (dw ? 16u : 20u) || (stride & 3)) { HVK_RED_ONCE("multi-draw stride %llu below the record size: drawn one record at a time", (unsigned long long)stride); maxn = 1; stride = dw ? 16 : 20; }
        g_mdi_cnt = mdi_cnt; g_mdi_skip_n = (int)maxn;
        if (dw) vkCmdDrawIndirectCount(cb, ab, aoff, cbuf, coff, maxn, (uint32_t)stride);
        else vkCmdDrawIndexedIndirectCount(cb, ab, aoff, cbuf, coff, maxn, (uint32_t)stride);
        return;
    }
    if (dw) vkCmdDrawIndirect(cb, ab, aoff, 1, 16); else vkCmdDrawIndexedIndirect(cb, ab, aoff, 1, 20);
}

// ---------- Dispatches ----------
void hvk_dispatch(const p_dispatch *dp);
void hvk_dispatch(const p_dispatch *dp) {
    if (g_cur < 0) return;
    if (!S.cprog) { HVK_RED_ONCE("dispatch with no compute program: skipped"); return; }
    hvk_prog *pr = hvk_prog_get(S.cprog);
    if (!pr || !pr->sh[5]) { HVK_RED_ONCE("dispatch with no compute shader (program ser=%llu): skipped", (unsigned long long)S.cprog->serial); return; }
    hvk_shader *sh[6] = {NULL, NULL, NULL, NULL, NULL, pr->sh[5]};
    hvk_layout *l = hvk_layout_for(sh); if (!l) return;
    VkPipeline pipe = hvk_cs_pipeline(pr->sh[5], l); if (!pipe) return;
    VkCommandBuffer cb = outside();
    uint32_t dim[3] = {(uint32_t)dp->x, (uint32_t)dp->y, (uint32_t)dp->z};
    if (dp->ind && pr->sh[5]->rf.drv_read[0]) HVK_RED_ONCE("an indirect dispatch whose shader reads its workgroup counts from c0: they read 0 (the GPU copy of the counts is phase P4)");
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe); g_bound.valid = 0;
    if (!hvk_bind_resources(cb, l, sh, VK_PIPELINE_BIND_POINT_COMPUTE, 0, dim)) return;
    atomic_fetch_add(&hvk.st_dispatches, 1);
    if (dp->ind) {
        VkBuffer ab; VkDeviceSize ao;
        if (!hvk_buf(dp->ind, 12, &ab, &ao, NULL) || (ao & 3)) { HVK_RED_ONCE("indirect dispatch arguments in no memory pool: skipped"); return; }
        vkCmdDispatchIndirect(cb, ab, ao);
    } else if (dp->x > 0 && dp->y > 0 && dp->z > 0) vkCmdDispatch(cb, dim[0], dim[1], dim[2]);
    g_need_barrier = 1;
}

// ---------- Clears ----------
static int scissor_full(uint32_t w, uint32_t h) { return !R.has_sc || (R.sc.x <= 0 && R.sc.y <= 0 && (int64_t)R.sc.x + R.sc.w >= (int64_t)w && (int64_t)R.sc.y + R.sc.h >= (int64_t)h); }
static uint32_t chmask_of(VkFormat f) {   // the channels a format has (a partial NVN mask over only those is still a full clear)
    switch (f) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: return 3;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16: return 7;
        default: return 15;
    }
}
static VkRect2D clear_rect(void) {
    uint32_t tw = P.extent.width, th = P.extent.height;
    if (!R.has_sc) return (VkRect2D){{0, 0}, {tw, th}};
    long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
    if (x1 > (long)tw) x1 = (long)tw;
    if (y1 > (long)th) y1 = (long)th;
    if (x1 <= x0 || y1 <= y0) return (VkRect2D){{0, 0}, {0, 0}};
    return (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}
static uint8_t kind_of_cls(uint8_t cls) { return cls == NVN_VK_UINT ? 1 : cls == NVN_VK_SINT ? 2 : 0; }
static void value_of(uint8_t cls, const float rgba[4], uint32_t v[4]) {
    for (int i = 0; i < 4; i++) { float f = rgba[i]; int32_t si;
        if (cls == NVN_VK_UINT) v[i] = f > 0 ? (f < 4294967040.f ? (uint32_t)f : 0xffffffffu) : 0;
        else if (cls == NVN_VK_SINT) { si = !(f == f) ? 0 : f >= 2147483520.f ? INT32_MAX : f <= -2147483648.f ? INT32_MIN : (int32_t)f; memcpy(&v[i], &si, 4); }
        else memcpy(&v[i], &f, 4); }
}
void hvk_clear_color(const p_clear_color *c);
void hvk_clear_color(const p_clear_color *c) {
    if (g_cur < 0 || g_rc_skip) return;
    if (c->index < 0 || c->index >= 8 || !R.has_rt || c->index >= R.rt.n || !R.rt.color[c->index]) { HVK_RED_ONCE("colour clear of target %d, which is not attached", c->index); return; }
    int mask = c->mask & 0xf; if (!mask) return;
    hvk_tex *x = hvk_tex_get(R.rt.color[c->index]); if (!x) return;
    hvk_view v; if (!hvk_tex_view(x, R.rt.has_cview[c->index] ? &R.rt.cview[c->index] : NULL, HVK_VIEW_ATTACH, &v)) return;
    uint32_t fm = chmask_of(v.format);
    int full_mask = (mask & fm) == fm;
    if (!P.active && full_mask && scissor_full(v.w, v.h) && !g_cond_on) {   // a load operation of the next pass
        R.clear_c[c->index] = 1; R.cc[c->index] = (nvn_clear_color){c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]};
        return;
    }
    VkCommandBuffer cb = hvk_main();
    if (!pass_begin(cb)) return;
    if (!P.ca[c->index].imageView) { HVK_RED_ONCE("colour clear of slot %d, which the pass does not attach", c->index); return; }
    VkRect2D rr = clear_rect(); if (!rr.extent.width || !rr.extent.height) return;
    if (full_mask) {
        nvn_clear_color cc = {c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]};
        VkClearAttachment a = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c->index, clear_value_of(P.ccls[c->index], &cc)};
        VkClearRect cr = {rr, 0, P.layers};
        vkCmdClearAttachments(cb, 1, &a, 1, &cr);
        return;
    }
    uint32_t val[4]; value_of(P.ccls[c->index], c->rgba, val);
    if (hvk_clear_tri(cb, P.cfmt, P.nc, P.dfmt, P.sfmt, P.samples, c->index, kind_of_cls(P.ccls[c->index]), (uint32_t)mask, val, 0, 0, 0, 0, rr, P.extent)) { g_bound.valid = 0; g_dyn.valid = 0; }
    HVK_NOTE_ONCE("colour clear with a partial channel mask: drawn as a triangle (first case slot %d mask %#x)", c->index, mask);
}
void hvk_clear_ds(const p_clear_ds *c);
void hvk_clear_ds(const p_clear_ds *c) {
    if (g_cur < 0 || g_rc_skip) return;
    if (!R.has_rt || !R.rt.depth) return;
    unsigned sm = (unsigned)c->stencil_mask & 0xff;
    if (!c->depth_mask && !sm) return;
    hvk_tex *x = hvk_tex_get(R.rt.depth); if (!x) return;
    hvk_view v; if (!hvk_tex_view(x, R.rt.has_dview ? &R.rt.dview : NULL, HVK_VIEW_ATTACH, &v)) return;
    int partial_s = sm && sm != 0xff && (v.aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    if (!P.active && scissor_full(v.w, v.h) && !partial_s && !g_cond_on) {   // a load operation of the next pass (every stencil bit, the whole attachment)
        if (c->depth_mask) { R.clear_d = 1; R.cd = c->depth; }
        if (sm) { R.clear_s = 1; R.cs = (uint32_t)c->stencil; }
        return;
    }
    VkCommandBuffer cb = hvk_main();
    if (!pass_begin(cb)) return;
    VkRect2D rr = clear_rect(); if (!rr.extent.width || !rr.extent.height) return;
    float z = c->depth > 0 ? (c->depth < 1 ? c->depth : 1) : 0;
    if (partial_s) {   // the stencil bits of the mask only: the clear triangle (depth written with it when asked)
        static const uint32_t zv[4];
        if (hvk_clear_tri(cb, P.cfmt, P.nc, P.dfmt, P.sfmt, P.samples, -1, 0, 0, zv, z, c->depth_mask && P.has_d, sm, (uint32_t)c->stencil & 0xff, rr, P.extent)) { g_bound.valid = 0; g_dyn.valid = 0; }
        return;
    }
    VkClearAttachment a = {0, 0, {{{0}}}};
    if (c->depth_mask && P.has_d) { a.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT; a.clearValue.depthStencil.depth = hvk_depth_clear_value(P.dfmt, z); }
    if (sm && P.has_s) { a.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT; a.clearValue.depthStencil.stencil = (uint32_t)c->stencil & 0xff; }
    if (!a.aspectMask) return;
    VkClearRect cr = {rr, 0, P.layers};
    vkCmdClearAttachments(cb, 1, &a, 1, &cr);
}
// ClearTexture: its region and mask, independently of the draw scissor (t_clear_tex); a whole level of every channel is vkCmdClear*Image, anything else a pass of its own
void hvk_clear_tex(const p_clear_tex *c);
void hvk_clear_tex(const p_clear_tex *c) {
    if (g_cur < 0 || !c->tex) return;
    hvk_tex *x = hvk_tex_get(c->tex); if (!x) return;
    VkFormat ifmt; VkImageAspectFlags asp; uint32_t w, h, d, layers, levels; VkSampleCountFlagBits samples; uint8_t cls;
    hvk_tex_info(x, &ifmt, &asp, &w, &h, &d, &layers, &levels, &samples, &cls);
    unsigned lv = c->has_view && c->view.set_levels ? c->view.base_level : 0, l0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    if (lv >= levels) return;
    uint32_t lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1;
    int nz = c->r.depth > 0 ? c->r.depth : 1, is3d = c->tex->d.target == NVN_TEXTURE_TARGET_3D;
    VkCommandBuffer cb = outside();
    hvk_tex_gpu_written(x);
    hvk_view v; view_t vv = c->has_view ? c->view : (view_t){0};
    int fmt_view = c->has_view && c->view.set_format && c->view.format != c->tex->d.format;
    int whole = !c->r.xoffset && !c->r.yoffset && (uint32_t)c->r.width >= lw && (uint32_t)c->r.height >= lh;
    if (asp & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        VkClearDepthStencilValue dv = {0, 0};
        if (asp & VK_IMAGE_ASPECT_DEPTH_BIT) { dv.depth = hvk_depth_clear_value(ifmt, c->c.f[0]); if (asp & VK_IMAGE_ASPECT_STENCIL_BIT) dv.stencil = c->c.u[1] & 0xff; }
        else dv.stencil = c->c.u[0] & 0xff;
        if (whole) {
            VkImageSubresourceRange r = {asp, lv, 1, is3d ? 0 : l0 + (uint32_t)c->r.zoffset, is3d ? 1 : (uint32_t)nz};
            vkCmdClearDepthStencilImage(cb, hvk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, &dv, 1, &r);
        } else HVK_RED_ONCE("texture clear of part of a depth / stencil level: not done (first case %dx%d of %ux%u)", c->r.width, c->r.height, lw, lh);
        g_need_barrier = 1; return;
    }
    uint32_t fm = chmask_of(ifmt), mask = (uint32_t)c->mask & fm;
    if (!mask) return;
    VkClearColorValue cv; memset(&cv, 0, sizeof cv);
    uint8_t vcls = cls;
    if (fmt_view) { nvn_vk_fmt row; nvn_fmt_to_vk(c->view.format, &row); vcls = row.cls; }
    for (int i = 0; i < 4; i++) {
        if (vcls == NVN_VK_FLOAT) cv.float32[i] = c->is_uint ? (float)c->c.u[i] : c->c.f[i];
        else if (c->is_uint) cv.uint32[i] = c->c.u[i];
        else { float f[4]; memcpy(f, c->c.f, 16); uint32_t u[4]; value_of(vcls, f, u); cv.uint32[i] = u[i]; }
    }
    if (whole && mask == fm && !fmt_view) {
        VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 1, is3d ? 0 : l0 + (uint32_t)c->r.zoffset, is3d ? 1 : (uint32_t)nz};
        if (is3d && (c->r.zoffset || (uint32_t)nz != (d >> lv ? d >> lv : 1))) HVK_RED_ONCE("texture clear of some slices of a 3D level: the whole level is cleared");
        vkCmdClearColorImage(cb, hvk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &r);
        g_need_barrier = 1; return;
    }
    // a pass per layer over that layer's level, the region as the scissor
    for (int z = 0; z < nz; z++) {
        vv.set_levels = 1; vv.base_level = (uint8_t)lv; vv.nlevels = 1; vv.set_layers = 1; vv.min_layer = (uint16_t)(l0 + (unsigned)c->r.zoffset + (unsigned)z); vv.nlayers = 1;
        if (!hvk_tex_view(x, &vv, HVK_VIEW_ATTACH, &v)) return;
        VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, v.view, VK_IMAGE_LAYOUT_GENERAL, 0, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {v.w, v.h}}, 1, 0, 1, &a, NULL, NULL};
        hvk_barrier_if_needed(cb);
        vkCmdBeginRendering(cb, &ri);
        long x0 = c->r.xoffset > 0 ? c->r.xoffset : 0, y0 = c->r.yoffset > 0 ? c->r.yoffset : 0, x1 = (long)c->r.xoffset + c->r.width, y1 = (long)c->r.yoffset + c->r.height;
        if (x1 > (long)v.w) x1 = (long)v.w;
        if (y1 > (long)v.h) y1 = (long)v.h;
        if (x1 > x0 && y1 > y0) {
            VkRect2D rr = {{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
            if (mask == fm) { VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = cv}}; VkClearRect cr = {rr, 0, 1}; vkCmdClearAttachments(cb, 1, &ca, 1, &cr); }
            else { VkFormat f8[8] = {v.format}; uint32_t val[4]; memcpy(val, cv.uint32, 16);
                   hvk_clear_tri(cb, f8, 1, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, v.samples, 0, kind_of_cls(v.cls), mask, val, 0, 0, 0, 0, rr, (VkExtent2D){v.w, v.h}); }
        }
        vkCmdEndRendering(cb);
        g_need_barrier = 1;
    }
    g_bound.valid = 0; g_dyn.valid = 0;
}

// ---------- Copies (4.3) ----------
void hvk_copy_b2b(uint64_t src, uint64_t dst, uint64_t n);
void hvk_copy_b2b(uint64_t src, uint64_t dst, uint64_t n) {
    if (g_cur < 0 || !n) return;
    VkBuffer sb, db; VkDeviceSize so, dof;
    if (!hvk_buf(src, n, &sb, &so, NULL) || !hvk_buf(dst, n, &db, &dof, NULL)) { HVK_RED_ONCE("buffer copy %#llx → %#llx (%llu bytes) outside the memory pools: skipped", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n); return; }
    VkCommandBuffer cb = outside();
    nvn_gw_pend_add(dst, n, 2);
    if (src < dst + n && dst < src + n) {   // overlapping ranges (the same pages, possibly through two imports): through the ring, two ordered copies
        hvk_alloc a; if (!hvk_ring_alloc(n, 16, &a)) return;
        VkBufferCopy c1 = {so, a.off, n}; vkCmdCopyBuffer(cb, sb, a.buf, 1, &c1);
        hvk_full_barrier(cb);
        VkBufferCopy c2 = {a.off, dof, n}; vkCmdCopyBuffer(cb, a.buf, db, 1, &c2);
    } else { VkBufferCopy c = {so, dof, n}; vkCmdCopyBuffer(cb, sb, db, 1, &c); }
    g_need_barrier = 1;
}
void hvk_update_ub(const void *data, uint64_t dst, uint64_t n);
void hvk_update_ub(const void *data, uint64_t dst, uint64_t n) {
    if (g_cur < 0 || !n) return;
    VkBuffer db; VkDeviceSize dof;
    if (!hvk_buf(dst, n, &db, &dof, NULL)) { HVK_RED_ONCE("inline uniform update at %#llx outside the memory pools: skipped", (unsigned long long)dst); return; }
    hvk_alloc a; if (!hvk_ring_alloc(n, 16, &a)) return;
    memcpy(a.cpu, data, n);
    int e = nvn_gw_pend_add(dst, n, 1); if (e >= 0) nvn_gw_pend_data(e, dst, a.cpu);   // a bindless handle it writes is read as the GPU will see it (nvn_resolve.c)
    // an update inside a pass ends it: copies cannot run in a pass (4.9; the uniform deferral and shadows of the Metal backend come to the frontend in phase P4)
    if (P.active) atomic_fetch_add(&hvk.st_pass_splits, 1);
    VkCommandBuffer cb = outside();
    VkBufferCopy c = {a.off, dof, n}; vkCmdCopyBuffer(cb, a.buf, db, 1, &c);
    g_need_barrier = 1;
}
void hvk_fill(uint64_t dst, uint64_t n, uint32_t value);
void hvk_fill(uint64_t dst, uint64_t n, uint32_t value) {
    if (g_cur < 0 || !n) return;
    VkBuffer db; VkDeviceSize dof;
    if (!hvk_buf(dst, n, &db, &dof, NULL)) { HVK_RED_ONCE("buffer fill at %#llx outside the memory pools: skipped", (unsigned long long)dst); return; }
    VkCommandBuffer cb = outside();
    nvn_gw_pend_add(dst, n, 3);
    if (!(dof & 3) && !(n & 3)) vkCmdFillBuffer(cb, db, dof, n, value);
    else {   // not 4-byte aligned: the pattern from the ring
        hvk_alloc a; if (!hvk_ring_alloc(n + 4, 16, &a)) return;
        for (uint64_t i = 0; i < n; i++) a.cpu[i] = (uint8_t)(value >> (8 * ((dst + i) & 3)));
        VkBufferCopy c = {a.off, dof, n}; vkCmdCopyBuffer(cb, a.buf, db, 1, &c);
    }
    g_need_barrier = 1;
}
static void level_size(hvk_tex *x, unsigned level, uint32_t *w, uint32_t *h, uint32_t *d) {
    uint32_t W, H, D; hvk_tex_info(x, NULL, NULL, &W, &H, &D, NULL, NULL, NULL, NULL);
    *w = W >> level ? W >> level : 1; *h = H >> level ? H >> level : 1; *d = D >> level ? D >> level : 1;
}
static VkImageAspectFlags copy_aspect(VkImageAspectFlags a, const char *what) {
    if ((a & VK_IMAGE_ASPECT_DEPTH_BIT) && (a & VK_IMAGE_ASPECT_STENCIL_BIT)) { HVK_RED_ONCE("%s of a combined depth-stencil texture: the depth plane only (the packing of D24S8 / D32FS8 in guest memory is phase P7)", what); return VK_IMAGE_ASPECT_DEPTH_BIT; }
    return a;
}
void hvk_copy_b2t(const p_copy_b2t *c, int to_buffer);
void hvk_copy_b2t(const p_copy_b2t *c, int to_buffer) {
    if (g_cur < 0 || !c->tex) return;
    hvk_tex *x = hvk_tex_get(c->tex); if (!x) return;
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    if (!bpb) return;
    unsigned level = c->has_view && c->view.set_levels ? c->view.base_level : 0, layer0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((c->r.width + bw - 1) / bw) * (size_t)bpb;
    size_t rows = (size_t)((c->r.height + bh - 1) / bh), img = S.copy_img_stride ? (size_t)S.copy_img_stride : row * rows;
    int is3d = c->tex->d.target == NVN_TEXTURE_TARGET_3D, nz = c->r.depth > 0 ? c->r.depth : 1;
    VkBuffer b; VkDeviceSize off;
    if (!hvk_buf(c->src, img * (size_t)nz, &b, &off, NULL)) { HVK_RED_ONCE("%s copy with a buffer outside the memory pools: skipped", to_buffer ? "texture→buffer" : "buffer→texture"); return; }
    VkFormat f; VkImageAspectFlags asp; hvk_tex_info(x, &f, &asp, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    asp = copy_aspect(asp, to_buffer ? "texture→buffer copy" : "buffer→texture copy");
    if (row % (size_t)bpb || off % (VkDeviceSize)(bpb % 4 ? (bpb == 2 ? 2 : bpb == 1 ? 1 : bpb) : 4)) {
        HVK_RED_ONCE("%s copy with a row stride (%llu) or offset not a multiple of the texel size (%d): skipped", to_buffer ? "texture→buffer" : "buffer→texture", (unsigned long long)row, bpb); return; }
    VkCommandBuffer cb = outside();
    VkBufferImageCopy r = {off, (uint32_t)(row / (size_t)bpb * (size_t)bw), (uint32_t)(img / row * (size_t)bh), {asp, level, layer0 + (uint32_t)c->r.zoffset, (uint32_t)nz},
                           {c->r.xoffset, c->r.yoffset, 0}, {(uint32_t)c->r.width, (uint32_t)c->r.height, 1}};
    if (is3d) { r.imageSubresource.baseArrayLayer = 0; r.imageSubresource.layerCount = 1; r.imageOffset.z = c->r.zoffset; r.imageExtent.depth = (uint32_t)nz; }
    if (to_buffer) { vkCmdCopyImageToBuffer(cb, hvk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, b, 1, &r); nvn_gw_sbw_note(c->src, img * (size_t)nz, SBW_GPU); }
    else { vkCmdCopyBufferToImage(cb, b, hvk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, 1, &r); hvk_tex_gpu_written(x); }
    g_need_barrier = 1;
}
void hvk_copy_t2t(const p_copy_t2t *c);
void hvk_copy_t2t(const p_copy_t2t *c) {
    if (g_cur < 0 || !c->src || !c->dst) return;
    hvk_tex *s = hvk_tex_get(c->src), *d = hvk_tex_get(c->dst); if (!s || !d) return;
    unsigned sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, ss = c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0;
    unsigned dl = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0, ds = c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0;
    VkFormat sf, df; VkImageAspectFlags sa, da; VkSampleCountFlagBits sn, dn; uint32_t slevels, dlevels;
    hvk_tex_info(s, &sf, &sa, NULL, NULL, NULL, NULL, &slevels, &sn, NULL); hvk_tex_info(d, &df, &da, NULL, NULL, NULL, NULL, &dlevels, &dn, NULL);
    if (sl >= slevels || dl >= dlevels) return;
    int s3 = c->src->d.target == NVN_TEXTURE_TARGET_3D, d3 = c->dst->d.target == NVN_TEXTURE_TARGET_3D;
    int nz = c->sr.depth > 0 ? c->sr.depth : 1;
    VkCommandBuffer cb = outside();
    hvk_tex_gpu_written(d);
    int mirror = c->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT);
    int scaled = c->sr.width != c->dr.width || c->sr.height != c->dr.height;
    int sbpb = nvn_fmt_bpb(c->src->d.format, NULL, NULL), dbpb = nvn_fmt_bpb(c->dst->d.format, NULL, NULL);
    VkImageSubresourceLayers srl = {copy_aspect(sa, "texture copy"), sl, s3 ? 0 : ss + (uint32_t)c->sr.zoffset, s3 ? 1 : (uint32_t)nz};
    VkImageSubresourceLayers drl = {copy_aspect(da, "texture copy"), dl, d3 ? 0 : ds + (uint32_t)c->dr.zoffset, d3 ? 1 : (uint32_t)nz};
    if ((c->flags & DM_COPY_RESOLVE) && sn > 1) {   // Downsample of a multisampled texture: a resolve
        VkImageResolve r = {srl, {c->sr.xoffset, c->sr.yoffset, 0}, drl, {c->dr.xoffset, c->dr.yoffset, 0}, {(uint32_t)c->sr.width, (uint32_t)c->sr.height, 1}};
        if (sf != df) HVK_RED_ONCE("multisample resolve between formats %u and %u: Vulkan resolves within one format (done as it is)", (unsigned)sf, (unsigned)df);
        vkCmdResolveImage(cb, hvk_tex_image(s), VK_IMAGE_LAYOUT_GENERAL, hvk_tex_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        g_need_barrier = 1; return;
    }
    if (!mirror && !scaled && sbpb == dbpb && sn == dn && (sf == df || !((sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)))) {
        if (sf != df && c->src->d.format != c->dst->d.format) {
            nvn_vk_fmt a, b; nvn_fmt_to_vk(c->src->d.format, &a); nvn_fmt_to_vk(c->dst->d.format, &b);
            if ((a.flags & NVN_VK_SRGB) != (b.flags & NVN_VK_SRGB) && !(a.flags & NVN_VK_BLOCK)) goto blit;   // a linear ↔ sRGB copy converts (the Metal backend draws it: t_readback's sRGB case)
        }
        VkImageCopy r = {srl, {c->sr.xoffset, c->sr.yoffset, s3 ? c->sr.zoffset : 0}, drl, {c->dr.xoffset, c->dr.yoffset, d3 ? c->dr.zoffset : 0},
                         {(uint32_t)c->sr.width, (uint32_t)c->sr.height, s3 || d3 ? (uint32_t)nz : 1}};
        if (s3 != d3) { HVK_RED_ONCE("texture copy between a 3D and a layered texture: skipped"); return; }
        vkCmdCopyImage(cb, hvk_tex_image(s), VK_IMAGE_LAYOUT_GENERAL, hvk_tex_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        g_need_barrier = 1; return;
    }
blit:;
    VkFormatProperties sp, dp; vkGetPhysicalDeviceFormatProperties(hvk.dev.phys, sf, &sp); vkGetPhysicalDeviceFormatProperties(hvk.dev.phys, df, &dp);
    int linear = (c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) != 0;
    int can = (sp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (dp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) && sn == 1 && dn == 1 &&
              (!linear || (sp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT));
    uint8_t scl = 0, dcl = 0; hvk_tex_info(s, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &scl); hvk_tex_info(d, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &dcl);
    if ((scl == NVN_VK_UINT || scl == NVN_VK_SINT || dcl == NVN_VK_UINT || dcl == NVN_VK_SINT) && scl != dcl) can = 0;   // integer ↔ other classes: no blit
    if ((sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) { if (sf != df || scaled) can = 0; linear = 0; }
    if (!can) { HVK_RED_ONCE("texture copy %ux%u format %u → %ux%u format %u%s: needs a draw-based copy (design 2.7, phase P4), skipped", c->sr.width, c->sr.height, (unsigned)sf, c->dr.width, c->dr.height, (unsigned)df, mirror ? " mirrored" : ""); return; }
    VkImageBlit r = {srl, {{c->sr.xoffset, c->sr.yoffset, 0}, {c->sr.xoffset + c->sr.width, c->sr.yoffset + c->sr.height, 1}},
                     drl, {{c->dr.xoffset, c->dr.yoffset, 0}, {c->dr.xoffset + c->dr.width, c->dr.yoffset + c->dr.height, 1}}};
    if (c->flags & NVN_COPY_FLAGS_MIRROR_X_BIT) { int32_t t = r.srcOffsets[0].x; r.srcOffsets[0].x = r.srcOffsets[1].x; r.srcOffsets[1].x = t; }
    if (c->flags & NVN_COPY_FLAGS_MIRROR_Y_BIT) { int32_t t = r.srcOffsets[0].y; r.srcOffsets[0].y = r.srcOffsets[1].y; r.srcOffsets[1].y = t; }
    if (s3 || d3) { r.srcOffsets[0].z = s3 ? c->sr.zoffset : 0; r.srcOffsets[1].z = r.srcOffsets[0].z + (s3 ? nz : 1); r.dstOffsets[0].z = d3 ? c->dr.zoffset : 0; r.dstOffsets[1].z = r.dstOffsets[0].z + (d3 ? nz : 1); }
    vkCmdBlitImage(cb, hvk_tex_image(s), VK_IMAGE_LAYOUT_GENERAL, hvk_tex_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &r, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    g_need_barrier = 1;
}

// ---------- Barriers, events, syncs, render targets ----------
void hvk_barrier(int32_t bits);
void hvk_barrier(int32_t bits) {
    if (g_cur < 0) return;
    // ordered at the next pass; inside a pass with storage writes the pass ends first, as the Metal backend's swrite_note (no barrier can run inside a dynamic-rendering pass in 1.3)
    if (P.active && P.swrite && (bits & (NVN_BARRIER_ORDER_PRIMITIVES_BIT | NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_ORDER_FRAGMENTS_TILED_BIT))) { pass_end_only(); atomic_fetch_add(&hvk.st_pass_splits, 1); }
    if (bits) g_need_barrier = 1;
}
void hvk_signal_event(volatile uint32_t *val, uint32_t mode, uint32_t v);
void hvk_signal_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { if (g_cur >= 0 && val) post_add((post_op){POST_EVENT, val, mode, v, 0, 0}); }   // a CPU write at completion, in order (4.5)
void hvk_set_rt(const p_set_rt *r);
void hvk_set_rt(const p_set_rt *r) {
    if (g_cur < 0) { R.rt = *r; R.has_rt = 1; return; }
    if (P.active && R.has_rt && R.rt.n == r->n && !clears_pending()) {   // the same targets: the pass goes on
        int same = 1;
        for (int i = 0; i < r->n && same; i++) same = (R.rt.color[i] ? R.rt.color[i]->serial : 0) == (r->color[i] ? r->color[i]->serial : 0) && R.rt.has_cview[i] == r->has_cview[i] &&
                                                      (!r->has_cview[i] || !memcmp(&R.rt.cview[i], &r->cview[i], sizeof r->cview[i]));
        same = same && (R.rt.depth ? R.rt.depth->serial : 0) == (r->depth ? r->depth->serial : 0) && R.rt.has_dview == r->has_dview && (!r->has_dview || !memcmp(&R.rt.dview, &r->dview, sizeof r->dview));
        if (same) { R.rt = *r; return; }
    }
    hvk_pass_end();   // the clears waiting for the old targets land on them first
    R.rt = *r; R.has_rt = 1;
}
// For tests (dm/gfx/tests/vk/t_vk_queue_origin.c): the raster state the last draw was given (VkCullModeFlags, VkFrontFace) and whether its viewport was flipped
void hvk_test_raster(uint32_t *cull, uint32_t *front, int *flipped);
void hvk_test_raster(uint32_t *cull, uint32_t *front, int *flipped) { *cull = g_dyn.cull; *front = g_dyn.front; *flipped = g_dyn.vp.height < 0; }
void hvk_state_dirty(uint32_t mask);
void hvk_state_dirty(uint32_t mask) { (void)mask; }   // the draw reads S itself and the pipeline cache is keyed by content: nothing to drop
