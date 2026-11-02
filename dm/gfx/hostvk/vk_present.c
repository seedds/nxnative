// The Vulkan backend's window presenter and vsync source (docs/design/vulkan-backend.md 7.2, 7.3, 7.4; step P4a). vk_present.h has the rules; vk_backend.c routes the be_* entries here.
// Threads: the game's thread presents (hvk_win_present: a snapshot copy, under g_mu); the present thread (present_main) makes the surface and the swapchain, acquires, blits (or runs
// the upscaler's compute passes, in the same command buffer: the game's thread never waits for them) and presents;
// the vsync thread (tick_main) ticks; with VK_KHR_present_wait the waiter (wait_main) learns when each present reached the screen. Locks: g_mu (the snapshots, the state, the ring of
// present times) is never held across a wait for the presentation engine, only for a snapshot's own previous copy and blit on the GPU; g_sc_mu serializes host access to the
// swapchain (acquire, present, the present waits, recreation) and is taken before g_mu where both are held; hvk.queue_mu (the VkQueue) is taken inside both, never the other way
// round. The presenter's timeline values are taken and submitted under hvk.queue_mu, so they rise in submission order whichever thread submits
#include "plat/plat.h"
#include "front.h"        // front_screen_max_fps: the window screen's refresh rate (front_sdl.c)
#include "vk_backend.h"
#include "vk_present.h"
#include "shaders/gen/fsr_easu_comp.h"   // the upscaler's two passes (shaders/fsr_easu.comp, fsr_rcas.comp over dm/third_party/fsr1)
#include "shaders/gen/fsr_rcas_comp.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SNAP_N = 3, FRAME_N = 3, IMG_MAX = 8, PRES_RING = 1024, PW_RING = 64 };
#define TICK_NS (1e9 / 60)                 // the vsync source's tick: the Switch's 60 Hz vsync
#define SNAP_WAIT_NS 100000000ull          // a present waits this long for a free snapshot before it drops its frame (the present thread is stuck: a hidden window, a dead surface)
#define ACQUIRE_NS 100000000ull            // an acquire waits this long before the frame is dropped (a window the compositor does not show)
#define SHARPEN_DEFAULT 0.2f               // RCAS's sharpness in stops when DM_PRESENT_SHARPEN is unset: AMD's recommended default (FSR 1.0 integration guide, "RCAS sharpening amount")
enum { UP_OFF, UP_FSR, UP_INTEGER };       // DM_PRESENT_UPSCALE
enum { SCALE_BLIT, SCALE_FSR_DIRECT, SCALE_FSR_COPY, SCALE_INTEGER };   // how one frame is scaled (hvk_win_info.upscale)

typedef struct {
    VkImage img; hvk_mem *mem; VkFormat fmt; uint32_t w, h;   // the snapshot: the frame's region in the frame's raw format (an sRGB format's UNORM twin)
    int sampled; VkImageView view; // made with SAMPLED usage (its format filters linearly); the view FSR's EASU samples, made by the present thread at its first use
    VkCommandBuffer cb;            // its copy's command buffer, free once `copied` is reached
    uint64_t copied, read;         // the timeline values of its copy and of the last blit that read it (the image is remade only once that is reached)
    uint64_t n;                    // the present number (be_presented_time's n)
    int flip;
} snap_t;
typedef struct { uint64_t n; double t; } pres_ent;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;      // the snapshots, the state below, the ring of present times
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_sc_mu = PTHREAD_MUTEX_INITIALIZER;   // host access to the swapchain
static hvk_surface_fn g_make_surface; static hvk_pixel_size_fn g_pixel_size;   // the window (hvk_present_set_window)
static struct {
    int wanted, device_ok, started, on, failed, stop;
    VkSurfaceKHR surface;
    VkSemaphore tl; uint64_t tl_n;                 // the presenter's timeline, its last value submitted (hvk.queue_mu)
    VkCommandPool pool;                            // the snapshots' and the present thread's command buffers (g_mu for the snapshots', the present thread alone for its own)
    snap_t snap[SNAP_N]; uint64_t wr, rd;          // snapshots queued and taken
    uint64_t present_n, last_ticket, dropped;
    pthread_t th, tick_th, wait_th; int th_on, tick_on, wait_on;
    // the present thread's
    VkCommandPool fpool; VkSwapchainKHR sc; uint64_t sc_gen; VkFormat fmt; VkColorSpaceKHR cs; VkPresentModeKHR mode; VkExtent2D ext; uint32_t nimg;
    VkImage img[IMG_MAX]; VkSemaphore done[IMG_MAX];
    VkSemaphore acq[FRAME_N]; VkCommandBuffer fcb[FRAME_N]; uint64_t fval[FRAME_N], frame;
    int recreate, extent_free; uint64_t swapchains, shown;   // extent_free: the surface takes the swapchain's size (no current extent): the front end's size decides
    // readback (tests)
    int readback; VkBuffer rb; hvk_mem *rb_mem; void *rb_map; VkDeviceSize rb_size;
    hvk_win_info last; uint64_t last_val;
    // the upscaler (design 7.4, decision D4): its settings (start, or hvk_win_upscale under g_sc_mu) and the present thread's objects
    int up, up_no_direct, up_failed; float sharpen;
    int sc_storage; VkImageView sc_view[IMG_MAX];  // the swapchain's images take RCAS's storage writes: their views
    int fsr_easu_ok, fsr_rcas_ok;                  // R8G8B8A8_UNORM has the features of the EASU image (sampled, storage) and of the second intermediate image (storage, blit source)
    VkSampler smp; VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline easu, rcas[2]; VkDescriptorPool dpool; VkDescriptorSet ds[FRAME_N][2];
    VkImage easu_img, rcas_img; hvk_mem *easu_mem, *rcas_mem; VkImageView easu_view, rcas_view; uint32_t easu_w, easu_h, rcas_w, rcas_h;
    // when presents reached the screen (be_presented_time)
    pres_ent ring[PRES_RING];
    uint64_t pw_q[PW_RING]; uint64_t pw_qgen[PW_RING]; uint64_t pw_wr, pw_rd;   // present ids for the waiter, with the swapchain generation they were presented on
} W;

// The vsync source
static pthread_mutex_t v_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t v_cv = PTHREAD_COND_INITIALIZER;
static struct {
    uint64_t n;                    // ticks so far
    uint64_t anchor;               // plat_now_ns of a refresh (0: none yet)
    uint64_t refresh_ns;           // VK_GOOGLE_display_timing's refresh duration (0: unknown)
    int kind;                      // the anchor's source: 1 an acquire that waited, 2 display timing, 3 present wait
    int on, stop;
} V;

void hvk_present_set_window(hvk_surface_fn make_surface, hvk_pixel_size_fn pixel_size) { g_make_surface = make_surface; g_pixel_size = pixel_size; }

// ---------- Small helpers ----------
static uint64_t tl_submit(VkCommandBuffer cb, const VkSemaphoreSubmitInfo *waits, uint32_t nw, VkSemaphore bin, VkResult *res) {
    pthread_mutex_lock(&hvk.queue_mu);
    uint64_t v = W.tl_n + 1;
    VkSemaphoreSubmitInfo sig[2] = {{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, W.tl, v, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0},
                                    {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, bin, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0}};
    VkCommandBufferSubmitInfo ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, cb, 0};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, nw, waits, 1, &ci, bin ? 2u : 1u, sig};
    VkResult r = vkQueueSubmit2(hvk.dev.queue, 1, &si, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) W.tl_n = v;
    pthread_mutex_unlock(&hvk.queue_mu);
    if (res) *res = r;
    return r == VK_SUCCESS ? v : 0;
}
static uint64_t tl_last(void) { pthread_mutex_lock(&hvk.queue_mu); uint64_t v = W.tl_n; pthread_mutex_unlock(&hvk.queue_mu); return v; }
static void tl_wait(uint64_t v) {
    if (!v || !W.tl) return;
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &W.tl, &v};
    uint64_t t0 = plat_now_ns();
    while (vkWaitSemaphores(hvk.d, &wi, 1000000000ull) == VK_TIMEOUT)
        if (plat_now_ns() - t0 > 5000000000ull) HVK_RED_ONCE("the window presenter has waited over 5 s for its own copy or blit (value %llu): the GPU is stuck?", (unsigned long long)v);
}
static int image_make(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImage *img, hvk_mem **mem) {   // its memory from the allocator (vk_alloc.h)
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt, {w, h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    *img = VK_NULL_HANDLE; *mem = NULL;
    if (vkCreateImage(hvk.d, &ci, NULL, img)) { *img = VK_NULL_HANDLE; return 0; }
    if (hvk_mem_image(*img, 0, 0, mem)) { vkDestroyImage(hvk.d, *img, NULL); *img = VK_NULL_HANDLE; *mem = NULL; return 0; }
    return 1;
}
static void image_free(VkImage *img, hvk_mem **mem) {
    if (*img) vkDestroyImage(hvk.d, *img, NULL);
    hvk_mem_free(*mem);
    *img = VK_NULL_HANDLE; *mem = NULL;
}
static VkImageView view_make(VkImage img, VkFormat fmt) {
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, fmt, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView v = VK_NULL_HANDLE;
    return vkCreateImageView(hvk.d, &ci, NULL, &v) ? VK_NULL_HANDLE : v;
}
static void view_free(VkImageView *v) { if (*v) vkDestroyImageView(hvk.d, *v, NULL); *v = VK_NULL_HANDLE; }
static void snap_free(snap_t *s) { view_free(&s->view); image_free(&s->img, &s->mem); s->sampled = 0; }
static VkFormatFeatureFlags fmt_features(VkFormat f) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(hvk.dev.phys, f, &fp); return fp.optimalTilingFeatures; }
// The format whose texels are the same bytes without the sRGB decode: the copy into the snapshot is raw (design 7.2: the screen gets the stored bytes, as the Switch scans them out)
static VkFormat raw_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default: return f;
    }
}
static int fmt_srgb(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32 || f == VK_FORMAT_R8G8B8_SRGB || f == VK_FORMAT_B8G8R8_SRGB; }
static void ring_put(uint64_t n, double t) { if (n) W.ring[n % PRES_RING] = (pres_ent){n, t}; }   // under g_mu

// ---------- The vsync source ----------
// The display timing's actual present times anchor the clock where their time base is known: CLOCK_MONOTONIC, plat_now_ns's clock, on Linux. On Windows (open) no driver documents
// it, so there the FIFO acquire anchors the clock, as on a device without display timing (show() says so once)
#if defined(_WIN32)
#define DISPLAY_TIMES 0
#else
#define DISPLAY_TIMES hvk.dev.x.display_timing
#endif
// A new anchor (a refresh's time on plat_now_ns's clock): the ticks are laid on it when the screen runs at a multiple of 60 Hz
static void anchor(uint64_t t, int kind) {
    pthread_mutex_lock(&v_mu);
    V.anchor = t; if (kind > V.kind || !V.kind) V.kind = kind;
    pthread_mutex_unlock(&v_mu);
}
static int screen_hz(void) {
    int hz = front_screen_max_fps();
    if (hz <= 0 && V.refresh_ns) hz = (int)lround(1e9 / (double)V.refresh_ns);
    return hz > 0 ? hz : 60;
}
static void *tick_main(void *arg) {
    (void)arg;
    plat_thread_set_name("vsync source");
    plat_qos_set_self(PLAT_QOS_USER_INTERACTIVE);   // the game paces on these ticks, as on the display link's
    double next = (double)plat_now_ns() + TICK_NS;
    uint64_t seen_anchor = 0;
    for (;;) {
        pthread_mutex_lock(&v_mu);
        if (V.stop) { pthread_mutex_unlock(&v_mu); break; }
        uint64_t a = V.anchor;
        pthread_mutex_unlock(&v_mu);
        int hz = screen_hz();
        if (a && a != seen_anchor && hz % 60 == 0) {   // phase-lock to the screen's refreshes: the tick on the anchor's grid that is due next
            seen_anchor = a;
            double now = (double)plat_now_ns(), k = ceil((now - (double)a) / TICK_NS), aligned = (double)a + k * TICK_NS;
            if (fabs(aligned - next) > 500000.0) next = aligned;   // within half a millisecond: the jitter of the anchor, left alone
        }
        double now = (double)plat_now_ns();
        if (next > now) plat_sleep_ns((uint64_t)(next - now));
        pthread_mutex_lock(&v_mu);
        V.n++;
        pthread_cond_broadcast(&v_cv);
        pthread_mutex_unlock(&v_mu);
        next += TICK_NS;
        now = (double)plat_now_ns();
        if (now > next + 4 * TICK_NS) next = now + TICK_NS;   // far behind (the machine stalled): no burst of ticks
    }
    return NULL;
}
static int ticks_wait(uint64_t *from, uint64_t step, uint64_t timeout_ns, int reset_far) {   // wait until V.n reaches *from + step (1), else 0 at the timeout
    uint64_t limit = plat_now_ns() + timeout_ns;
    pthread_mutex_lock(&v_mu);
    if (reset_far && (*from > V.n || V.n > *from + 4 * step)) *from = V.n;
    while (V.n < *from + step) {
        uint64_t now = plat_now_ns();
        if (now >= limit || V.stop || !V.on) { pthread_mutex_unlock(&v_mu); return 0; }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t left = limit - now; if (left > 20000000ull) left = 20000000ull;   // a bounded slice: the condition variable's clock is the realtime one
        ts.tv_nsec += (long)left; while (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&v_cv, &v_mu, &ts);
    }
    *from = V.n;
    pthread_mutex_unlock(&v_mu);
    return 1;
}
int hvk_vsync_active(void) { return V.on; }
int hvk_vsync_rate(void) { return 60; }
int hvk_vsync_stalled(void) { return 0; }   // the ticks come from a clock: they never go silent as a display link can (a present that stalls drops frames instead, hvk_win_present)
// The display link's rule (nvn_mtl.m mtl_link_wait_ticks): when a tick has passed since the previous frame started, go at once; otherwise wait for it
int hvk_vsync_wait_ticks(int ticks, uint64_t timeout_ns) {
    if (!V.on) return 0;
    static uint64_t started;   // the game's present thread only, as on Apple
    const uint64_t step = ticks < 1 ? 1 : (uint64_t)ticks;
    pthread_mutex_lock(&v_mu);
    if (V.n >= started + step) { started = V.n; pthread_mutex_unlock(&v_mu); return 1; }
    pthread_mutex_unlock(&v_mu);
    return ticks_wait(&started, step, timeout_ns, 0);
}
int hvk_vsync_wait(int vsyncs, uint64_t timeout_ns) { return hvk_vsync_wait_ticks(vsyncs < 1 ? 1 : vsyncs, timeout_ns); }   // 60 Hz ticks: one per vsync
// hle_sdk16.c's vsync event (nvn_mtl.m mtl_link_wait_vsync): 1 at the next tick after *seen (updated), 0 without the source or at the timeout
int hvk_vsync_wait_vsync(uint64_t *seen, uint64_t timeout_ns) { return V.on ? ticks_wait(seen, 1, timeout_ns, 1) : 0; }
int be_vsync_wait_vsync(uint64_t *seen, uint64_t timeout_ns);   // hle_sdk16.c's name for it (its weak default answers "no source")
int be_vsync_wait_vsync(uint64_t *seen, uint64_t timeout_ns) { return hvk_vsync_wait_vsync(seen, timeout_ns); }

// ---------- The swapchain (present thread) ----------
static const char *mode_name(VkPresentModeKHR m) {
    return m == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" : m == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : m == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : m == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "FIFO_RELAXED" : "other";
}
static void sc_destroy(void) {   // the swapchain and its semaphores, once nothing uses them (g_sc_mu held)
    if (!W.sc) return;
    pthread_mutex_lock(&hvk.queue_mu); vkDeviceWaitIdle(hvk.d); pthread_mutex_unlock(&hvk.queue_mu);   // the presents still pending wait for these semaphores
    for (uint32_t i = 0; i < W.nimg; i++) if (W.done[i]) { vkDestroySemaphore(hvk.d, W.done[i], NULL); W.done[i] = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < W.nimg; i++) view_free(&W.sc_view[i]);
    vkDestroySwapchainKHR(hvk.d, W.sc, NULL);
    W.sc = VK_NULL_HANDLE; W.nimg = 0;
}
// 1 made, 0 not now (the window has no size: minimized), -1 the window presenter cannot go on (logged)
static int sc_make(void) {
    int pw = 0, ph = 0;
    if (g_pixel_size && g_pixel_size(&pw, &ph)) pw = ph = 0;
    VkSurfaceCapabilitiesKHR c;
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(hvk.dev.phys, W.surface, &c);
    if (r) { hostvk_log("[RED] [hostvk] window presenter: the surface's capabilities could not be read (VkResult %d): frames are no longer shown", r); return -1; }
    VkExtent2D e = c.currentExtent;
    W.extent_free = e.width == UINT32_MAX;
    if (W.extent_free) {   // the window takes the swapchain's size (Wayland, headless surfaces): the front end's
        e.width = (uint32_t)(pw > 0 ? pw : 0); e.height = (uint32_t)(ph > 0 ? ph : 0);
        if (e.width && e.height) {
            if (e.width < c.minImageExtent.width) e.width = c.minImageExtent.width;
            if (e.height < c.minImageExtent.height) e.height = c.minImageExtent.height;
            if (e.width > c.maxImageExtent.width) e.width = c.maxImageExtent.width;
            if (e.height > c.maxImageExtent.height) e.height = c.maxImageExtent.height;
        }
    }
    if (!e.width || !e.height) return 0;
    if (!(c.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { hostvk_log("[RED] [hostvk] window presenter: the surface's images take no transfer writes: frames are not shown"); return -1; }
    uint32_t n = c.minImageCount > 3 ? c.minImageCount : 3;   // three images (design 7.2), minImageCount respected
    if (c.maxImageCount && n > c.maxImageCount) n = c.maxImageCount;
    if (n > IMG_MAX) { hostvk_log("[RED] [hostvk] window presenter: the surface needs %u images, more than %d", n, IMG_MAX); return -1; }
    if (!W.fmt) {   // the format and the present mode, once
        uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(hvk.dev.phys, W.surface, &nf, NULL);
        VkSurfaceFormatKHR *f = calloc(nf ? nf : 1, sizeof *f);
        if (!f || vkGetPhysicalDeviceSurfaceFormatsKHR(hvk.dev.phys, W.surface, &nf, f) < 0) nf = 0;
        static const VkFormat pref[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32};
        int pick = -1;
        for (size_t k = 0; k < sizeof pref / sizeof pref[0] && pick < 0; k++)
            for (uint32_t i = 0; i < nf; i++) if (f[i].format == pref[k] && f[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { pick = (int)i; break; }
        for (uint32_t i = 0; i < nf && pick < 0; i++) if (!fmt_srgb(f[i].format) && f[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) pick = (int)i;
        if (pick < 0 && nf) {
            pick = 0;
            hostvk_log("[RED] [hostvk] window presenter: the surface offers no UNORM format in SRGB_NONLINEAR, only format %d colour space %d: the stored bytes are encoded again, the picture is brighter", f[0].format, f[0].colorSpace);
        }
        if (pick < 0) { free(f); hostvk_log("[RED] [hostvk] window presenter: the surface lists no format"); return -1; }
        W.fmt = f[pick].format; W.cs = f[pick].colorSpace;
        free(f);
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(hvk.dev.phys, W.fmt, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) { hostvk_log("[RED] [hostvk] window presenter: the swapchain format %d takes no blits", W.fmt); return -1; }
        uint32_t nm = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(hvk.dev.phys, W.surface, &nm, NULL);
        VkPresentModeKHR m[16]; if (nm > 16) nm = 16;
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(hvk.dev.phys, W.surface, &nm, m) < 0) nm = 0;
        W.mode = VK_PRESENT_MODE_FIFO_KHR;   // every surface has FIFO
        const char *e2 = getenv("DM_VK_PRESENT_MODE");
        if (e2 && *e2 && strcmp(e2, "fifo")) {
            int mailbox = !strcmp(e2, "mailbox"), has = 0;
            for (uint32_t i = 0; i < nm; i++) has |= m[i] == VK_PRESENT_MODE_MAILBOX_KHR;
            if (mailbox && has) W.mode = VK_PRESENT_MODE_MAILBOX_KHR;
            else hostvk_log("[RED] [hostvk] DM_VK_PRESENT_MODE=%s: %s: FIFO", e2, mailbox ? "the surface has no MAILBOX" : "not fifo or mailbox (IMMEDIATE is not offered, design 7.3)");
        }
        char modes[96] = ""; for (uint32_t i = 0; i < nm; i++) snprintf(modes + strlen(modes), sizeof modes - strlen(modes), "%s%s", i ? " " : "", mode_name(m[i]));
        hostvk_log("[hostvk] window presenter: format %d colour space %d, present mode %s (the surface has %s), vsync source %s", W.fmt, W.cs, mode_name(W.mode), modes,
                                  hvk.dev.x.present_wait ? "VK_KHR_present_wait" : DISPLAY_TIMES ? "VK_GOOGLE_display_timing" : "the FIFO acquire (no present wait, no display timing)");
    }
    VkCompositeAlphaFlagBitsKHR alpha = (c.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                       : (VkCompositeAlphaFlagBitsKHR)(c.supportedCompositeAlpha & -c.supportedCompositeAlpha);
    VkImageUsageFlags use = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (c.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);   // TRANSFER_SRC: the tests' readback
    // FSR's RCAS writes the swapchain image itself where the surface takes storage writes, the format has the storage feature and the shader can name it (rgba8, or any format
    // with shaderStorageImageWriteWithoutFormat); otherwise it writes a second intermediate image that is blitted one to one. Asked only with DM_PRESENT_UPSCALE=fsr: a storage
    // usage can cost a swapchain image its framebuffer compression on some GPUs
    W.sc_storage = W.up == UP_FSR && !W.up_no_direct && (c.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT) && (fmt_features(W.fmt) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
                   && (W.fmt == VK_FORMAT_R8G8B8A8_UNORM || hvk.dev.x.storage_without_format);
    if (W.sc_storage) use |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkSwapchainKHR old = W.sc;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, W.surface, n, W.fmt, W.cs, e, 1, use, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                                   c.currentTransform, alpha, W.mode, VK_TRUE, old};
    VkSwapchainKHR sc;
    r = vkCreateSwapchainKHR(hvk.d, &ci, NULL, &sc);
    sc_destroy();   // the old one is retired either way
    if (r) { hostvk_log("[RED] [hostvk] window presenter: vkCreateSwapchainKHR %ux%u failed (VkResult %d): frames are not shown", e.width, e.height, r); return r == VK_ERROR_OUT_OF_DATE_KHR ? 0 : -1; }
    W.sc = sc; W.sc_gen++; W.ext = e; W.recreate = 0; W.swapchains++;
    uint32_t ni = 0; vkGetSwapchainImagesKHR(hvk.d, sc, &ni, NULL);
    if (ni > IMG_MAX) ni = IMG_MAX;
    vkGetSwapchainImagesKHR(hvk.d, sc, &ni, W.img);
    W.nimg = ni;
    for (uint32_t i = 0; W.sc_storage && i < ni; i++)
        if (!(W.sc_view[i] = view_make(W.img[i], W.fmt))) { W.sc_storage = 0; HVK_RED_ONCE("window presenter: no view of a swapchain image: FSR's RCAS writes the second intermediate image"); }
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
    for (uint32_t i = 0; i < ni; i++) if (vkCreateSemaphore(hvk.d, &si, NULL, &W.done[i])) { W.done[i] = VK_NULL_HANDLE; hostvk_log("[RED] [hostvk] window presenter: no semaphore"); return -1; }
    if (hvk.dev.x.display_timing) { VkRefreshCycleDurationGOOGLE rc; if (!vkGetRefreshCycleDurationGOOGLE(hvk.d, sc, &rc)) V.refresh_ns = rc.refreshDuration; }
    hostvk_log("[hostvk] window presenter: swapchain %llu: %ux%u, %u images, %s%s", (unsigned long long)W.swapchains, e.width, e.height, ni, mode_name(W.mode),
               W.up != UP_FSR ? "" : W.sc_storage ? ", storage (FSR's RCAS writes it)" : ", no storage (FSR's RCAS writes a second image, blitted one to one)");
    return 1;
}

// ---------- Scaling: the picture's rectangle, the upscaler (design 7.4, decision D4) ----------
// The picture's rectangle in the window: the region keeps its aspect, centred (the rest is the letterbox); front_sdl.c maps the mouse onto the centred 16:9 picture the same way
static void fit(uint32_t sw, uint32_t sh, VkExtent2D e, int32_t r[4]) {
    double s = fmin((double)e.width / sw, (double)e.height / sh);
    int32_t w = (int32_t)lround(sw * s), h = (int32_t)lround(sh * s);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > (int32_t)e.width) w = (int32_t)e.width;
    if (h > (int32_t)e.height) h = (int32_t)e.height;
    r[0] = ((int32_t)e.width - w) / 2; r[1] = ((int32_t)e.height - h) / 2; r[2] = w; r[3] = h;
}
// DM_PRESENT_UPSCALE: `off` (the default, as on Apple) the bilinear blit; `fsr` AMD FidelityFX Super Resolution 1.0, EASU (edge-adaptive upsampling) then RCAS (contrast-adaptive
// sharpening), two compute passes (shaders/fsr_easu.comp, fsr_rcas.comp over the unmodified headers of dm/third_party/fsr1), where the picture is shown larger than the game renders
// it: FSR 1 only upscales (EASU's kernel is made for scales of 1 and more; at one to one and below, the blit, as before); `metalfx` has no Vulkan counterpart and selects FSR 1, the
// spatial upscaler here; `integer` nearest neighbour at the largest whole multiple of the picture that fits the window, centred, the border black (pixel-art games), and the blit
// where none fits (a picture larger than the window). DM_PRESENT_SHARPEN: RCAS's sharpness in stops, FsrRcasCon's argument: 0.0 the sharpest RCAS gives, each whole stop halves
// it, up to 2.0 (AMD: "values above 2.0 won't make a visible difference"); default 0.2, AMD's recommended default. Both passes filter the stored bytes as UNORM values, without an
// sRGB decode or encode: the picture's own gamma space, which FSR 1 expects (the snapshot is an sRGB frame's UNORM twin; the swapchain a UNORM format)
static void up_parse(const char *up, const char *sh) {   // at start (under g_mu, before the present thread), or hvk_win_upscale (under g_sc_mu)
    W.up = UP_OFF; W.sharpen = SHARPEN_DEFAULT;
    if (up && *up && strcmp(up, "off")) {
        if (!strcmp(up, "fsr")) W.up = UP_FSR;
        else if (!strcmp(up, "metalfx")) { W.up = UP_FSR; hostvk_log("[hostvk] DM_PRESENT_UPSCALE=metalfx: MetalFX does not exist on the Vulkan backend: AMD FSR 1, the spatial upscaler here"); }
        else if (!strcmp(up, "integer")) W.up = UP_INTEGER;
        else hostvk_log("[RED] [hostvk] DM_PRESENT_UPSCALE=%s: fsr, integer, metalfx or off: the picture is scaled bilinearly (off)", up);
    }
    if (sh && *sh) {
        char *end; double v = strtod(sh, &end);
        if (end != sh && !*end && v >= 0.0 && v <= 2.0) W.sharpen = (float)v;
        else hostvk_log("[RED] [hostvk] DM_PRESENT_SHARPEN=%s: RCAS's sharpness in stops, 0.0 (the sharpest) to 2.0: %.2f", sh, SHARPEN_DEFAULT);
    }
    if (W.up == UP_FSR) hostvk_log("[hostvk] window presenter: upscaling (DM_PRESENT_UPSCALE) fsr: AMD FSR 1.0, EASU then RCAS at a sharpness of %.2f stops (DM_PRESENT_SHARPEN), where the picture is shown larger than the game renders it; the blit otherwise", W.sharpen);
    else if (W.up == UP_INTEGER) hostvk_log("[hostvk] window presenter: upscaling (DM_PRESENT_UPSCALE) integer: nearest at the largest whole multiple that fits, centred; the blit where none fits");
}
typedef struct { int32_t in_size[2], out_size[2], off[2], flip; float sharpness; } fsr_push;   // the push constants of fsr_easu.comp and fsr_rcas.comp (one layout)
static void fsr_free(void) {   // nothing in flight uses them
    view_free(&W.easu_view); image_free(&W.easu_img, &W.easu_mem); view_free(&W.rcas_view); image_free(&W.rcas_img, &W.rcas_mem);
    W.easu_w = W.easu_h = W.rcas_w = W.rcas_h = 0;
    if (W.easu) vkDestroyPipeline(hvk.d, W.easu, NULL);
    for (int i = 0; i < 2; i++) if (W.rcas[i]) vkDestroyPipeline(hvk.d, W.rcas[i], NULL);
    if (W.pl) vkDestroyPipelineLayout(hvk.d, W.pl, NULL);
    if (W.dpool) vkDestroyDescriptorPool(hvk.d, W.dpool, NULL);   // and its sets
    if (W.dsl) vkDestroyDescriptorSetLayout(hvk.d, W.dsl, NULL);
    if (W.smp) vkDestroySampler(hvk.d, W.smp, NULL);
    W.easu = W.rcas[0] = W.rcas[1] = VK_NULL_HANDLE; W.pl = VK_NULL_HANDLE; W.dpool = VK_NULL_HANDLE; W.dsl = VK_NULL_HANDLE; W.smp = VK_NULL_HANDLE;
    memset(W.ds, 0, sizeof W.ds);
}
static VkPipeline fsr_pipeline(const uint32_t *code, size_t bytes) {
    VkShaderModule m = hvk_module(code, bytes);
    VkPipeline p = VK_NULL_HANDLE;
    if (!m) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL}, W.pl, VK_NULL_HANDLE, -1};
    if (vkCreateComputePipelines(hvk.d, VK_NULL_HANDLE, 1, &ci, NULL, &p)) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(hvk.d, m, NULL);
    return p;
}
// The passes' objects, made at the first frame FSR scales (present thread): 1, or 0 when the device cannot run them (one red line; the frames are blitted from then on)
static int fsr_init(void) {
    if (W.easu) return 1;
    if (W.up_failed) return 0;
    VkFormatFeatureFlags ff = fmt_features(VK_FORMAT_R8G8B8A8_UNORM);
    const VkFormatFeatureFlags easu_need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, rcas_need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    W.fsr_easu_ok = (ff & easu_need) == easu_need; W.fsr_rcas_ok = (ff & rcas_need) == rcas_need;
    const char *why = NULL;
    if (!W.fsr_easu_ok) why = "the device does not sample and store R8G8B8A8_UNORM (the EASU image)";
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0, VK_FALSE, 1, VK_FALSE, VK_COMPARE_OP_ALWAYS, 0, 0,
                              VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_FALSE};
    if (!why && vkCreateSampler(hvk.d, &si, NULL, &W.smp)) { W.smp = VK_NULL_HANDLE; why = "no sampler"; }
    // binding 0: the pass's source (the snapshot for EASU, the EASU image for RCAS), through the linear clamp-to-edge sampler; binding 1: its target, a storage image
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &W.smp}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b};
    if (!why && vkCreateDescriptorSetLayout(hvk.d, &li, NULL, &W.dsl)) { W.dsl = VK_NULL_HANDLE; why = "no descriptor set layout"; }
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(fsr_push)};
    VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &W.dsl, 1, &pc};
    if (!why && vkCreatePipelineLayout(hvk.d, &pi, NULL, &W.pl)) { W.pl = VK_NULL_HANDLE; why = "no pipeline layout"; }
    // two sets per frame in flight (EASU's, RCAS's), written again once that frame's previous submission is done (show waits for it)
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * FRAME_N}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * FRAME_N}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2 * FRAME_N, 2, ps};
    if (!why && vkCreateDescriptorPool(hvk.d, &dpi, NULL, &W.dpool)) { W.dpool = VK_NULL_HANDLE; why = "no descriptor pool"; }
    VkDescriptorSetLayout layouts[2 * FRAME_N];
    for (int i = 0; i < 2 * FRAME_N; i++) layouts[i] = W.dsl;
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, W.dpool, 2 * FRAME_N, layouts};
    if (!why && vkAllocateDescriptorSets(hvk.d, &dai, &W.ds[0][0])) why = "no descriptor sets";
    if (!why && (!(W.easu = fsr_pipeline(fsr_easu_comp_spv, sizeof fsr_easu_comp_spv)) || !(W.rcas[0] = fsr_pipeline(fsr_rcas_comp_k0_spv, sizeof fsr_rcas_comp_k0_spv))))
        why = "its compute pipelines could not be made";
    if (!why && hvk.dev.x.storage_without_format && !(W.rcas[1] = fsr_pipeline(fsr_rcas_comp_k1_spv, sizeof fsr_rcas_comp_k1_spv))) why = "its compute pipelines could not be made";
    if (why) {
        hostvk_log("[RED] [hostvk] window presenter: DM_PRESENT_UPSCALE=fsr: %s: the picture is scaled bilinearly", why);
        W.up_failed = 1; fsr_free(); return 0;
    }
    return 1;
}
// The EASU image (the picture's rectangle, R8G8B8A8_UNORM: EASU stores, RCAS samples) and, without RCAS's writes into the swapchain image, the second image (RCAS stores, blitted
// one to one), remade on a new size once the frames in flight that use them are done. 0: not made (one red line; the frame is blitted)
static int fsr_images(uint32_t w, uint32_t h, int second) {
    if (W.easu_img && W.easu_w == w && W.easu_h == h && (!second || (W.rcas_img && W.rcas_w == w && W.rcas_h == h))) return 1;
    for (int i = 0; i < FRAME_N; i++) tl_wait(W.fval[i]);
    if (W.easu_img && (W.easu_w != w || W.easu_h != h)) { view_free(&W.easu_view); image_free(&W.easu_img, &W.easu_mem); }
    if (!W.easu_img) {
        if (!image_make(VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, &W.easu_img, &W.easu_mem)
            || !(W.easu_view = view_make(W.easu_img, VK_FORMAT_R8G8B8A8_UNORM))) {
            image_free(&W.easu_img, &W.easu_mem); HVK_RED_ONCE("window presenter: no EASU image of %ux%u: the picture is scaled bilinearly", w, h); return 0;
        }
        W.easu_w = w; W.easu_h = h;
    }
    if (second && W.rcas_img && (W.rcas_w != w || W.rcas_h != h)) { view_free(&W.rcas_view); image_free(&W.rcas_img, &W.rcas_mem); }
    if (second && !W.rcas_img) {
        if (!image_make(VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &W.rcas_img, &W.rcas_mem)
            || !(W.rcas_view = view_make(W.rcas_img, VK_FORMAT_R8G8B8A8_UNORM))) {
            image_free(&W.rcas_img, &W.rcas_mem); HVK_RED_ONCE("window presenter: no RCAS image of %ux%u: the picture is scaled bilinearly", w, h); return 0;
        }
        W.rcas_w = w; W.rcas_h = h;
    }
    return 1;
}
// How this frame is scaled (SCALE_*), into the rectangle rect of the window (fit's, or the integer multiple's)
static int scale_plan(snap_t *s, uint32_t idx, int32_t rect[4]) {
    fit(s->w, s->h, W.ext, rect);
    if (W.up == UP_INTEGER) {
        uint32_t kx = W.ext.width / s->w, ky = W.ext.height / s->h, k = kx < ky ? kx : ky;
        if (!k) return SCALE_BLIT;   // the picture is larger than the window: no whole multiple fits
        rect[2] = (int32_t)(s->w * k); rect[3] = (int32_t)(s->h * k);
        rect[0] = ((int32_t)W.ext.width - rect[2]) / 2; rect[1] = ((int32_t)W.ext.height - rect[3]) / 2;
        return SCALE_INTEGER;
    }
    if (W.up != UP_FSR || rect[2] < (int32_t)s->w || rect[3] < (int32_t)s->h || (rect[2] == (int32_t)s->w && rect[3] == (int32_t)s->h)) return SCALE_BLIT;   // FSR 1 only upscales
    if (!s->sampled) { HVK_RED_ONCE("window presenter: DM_PRESENT_UPSCALE=fsr: format %d cannot be sampled linearly: such frames are scaled bilinearly", s->fmt); return SCALE_BLIT; }
    if (!fsr_init()) return SCALE_BLIT;
    int direct = W.sc_storage && W.sc_view[idx] && (W.fmt == VK_FORMAT_R8G8B8A8_UNORM || W.rcas[1]);
    if (!direct && !W.fsr_rcas_ok) { HVK_RED_ONCE("window presenter: DM_PRESENT_UPSCALE=fsr: neither the swapchain image nor an R8G8B8A8_UNORM image takes RCAS's writes and a blit: the picture is scaled bilinearly"); return SCALE_BLIT; }
    if (!s->view && !(s->view = view_make(s->img, s->fmt))) { HVK_RED_ONCE("window presenter: no view of the snapshot: the picture is scaled bilinearly"); return SCALE_BLIT; }
    if (!fsr_images((uint32_t)rect[2], (uint32_t)rect[3], !direct)) return SCALE_BLIT;
    return direct ? SCALE_FSR_DIRECT : SCALE_FSR_COPY;
}
// FSR 1 from the snapshot into the picture's rectangle of the swapchain image dst (in TRANSFER_DST_OPTIMAL, the letterbox cleared): EASU into the EASU image, RCAS into dst
// (direct) or into the second image, blitted one to one. dst's layout and its last write's stage and access come back for the barriers that follow
static void fsr_record(VkCommandBuffer cb, uint32_t f, snap_t *s, VkImage dst, uint32_t idx, const int32_t rect[4], int direct, VkImageLayout *layout, VkPipelineStageFlags2 *stage,
                       VkAccessFlags2 *access) {
    const VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDescriptorImageInfo ii[4] = {{VK_NULL_HANDLE, s->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {VK_NULL_HANDLE, W.easu_view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, W.easu_view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, direct ? W.sc_view[idx] : W.rcas_view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[4];
    for (int i = 0; i < 4; i++)
        wr[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, W.ds[f][i / 2], (uint32_t)(i % 2), 0, 1,
                                       i % 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[i], NULL, NULL};
    vkUpdateDescriptorSets(hvk.d, 4, wr, 0, NULL);   // the sets of frame f: its previous submission is done (show waited for W.fval[f])
    // The snapshot for EASU's taps (its copy is earlier on the queue); the EASU image discarded for EASU's writes (an earlier frame's RCAS read it)
    VkImageMemoryBarrier2 b1[2] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, s->img, whole},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, W.easu_img, whole}};
    VkDependencyInfo d1 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 2, b1};
    vkCmdPipelineBarrier2(cb, &d1);
    fsr_push pc = {{(int32_t)s->w, (int32_t)s->h}, {rect[2], rect[3]}, {direct ? rect[0] : 0, direct ? rect[1] : 0}, s->flip, W.sharpen};
    const uint32_t gx = ((uint32_t)rect[2] + 15) / 16, gy = ((uint32_t)rect[3] + 15) / 16;   // a 16 x 16 tile per workgroup
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, W.easu);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, W.pl, 0, 1, &W.ds[f][0], 0, NULL);
    vkCmdPushConstants(cb, W.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, gx, gy, 1);
    // EASU's output for RCAS's reads; RCAS's target for its writes: the swapchain image (after the letterbox's clear) or the second image (discarded: an earlier frame's blit read it)
    VkImageMemoryBarrier2 b2[2] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, W.easu_img, whole},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, direct ? VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, direct ? VK_ACCESS_2_TRANSFER_WRITE_BIT : 0,
         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, direct ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, direct ? dst : W.rcas_img, whole}};
    VkDependencyInfo d2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 2, b2};
    vkCmdPipelineBarrier2(cb, &d2);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, direct && W.fmt != VK_FORMAT_R8G8B8A8_UNORM ? W.rcas[1] : W.rcas[0]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, W.pl, 0, 1, &W.ds[f][1], 0, NULL);
    vkCmdDispatch(cb, gx, gy, 1);   // the push constants stay bound (one layout)
    if (direct) { *layout = VK_IMAGE_LAYOUT_GENERAL; *stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; *access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT; return; }
    // The second image into the picture's rectangle, one to one (the blit converts RGBA to the swapchain's byte order)
    VkImageMemoryBarrier2 b3 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, W.rcas_img, whole};
    VkDependencyInfo d3 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b3};
    vkCmdPipelineBarrier2(cb, &d3);
    VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {rect[2], rect[3], 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{rect[0], rect[1], 0}, {rect[0] + rect[2], rect[1] + rect[3], 1}}};
    vkCmdBlitImage(cb, W.rcas_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    *layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; *stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; *access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
}

// ---------- The present thread ----------
static void drop_snapshot(snap_t *s) {   // a frame that is not shown: its present time is -1 (dropped), as Metal records a frame the system dropped
    pthread_mutex_lock(&g_mu); W.dropped++; ring_put(s->n, -1); W.rd++; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
}
// One snapshot onto the screen. 1 shown, 0 dropped, -1 the presenter cannot go on
static int show(snap_t *s) {
    pthread_mutex_lock(&g_sc_mu);
    int pw = 0, ph = 0;
    // A surface with a current extent reports a new size itself (OUT_OF_DATE, SUBOPTIMAL); one without (Wayland, a headless surface) takes the front end's: a new one remakes it
    if (W.extent_free && g_pixel_size && !g_pixel_size(&pw, &ph) && W.sc && ((uint32_t)pw != W.ext.width || (uint32_t)ph != W.ext.height)) W.recreate = 1;
    uint32_t idx = 0; uint32_t f = (uint32_t)(W.frame % FRAME_N);
    tl_wait(W.fval[f]);   // its acquire semaphore and command buffer are free
    VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
    for (int tries = 0; tries < 2; tries++) {
        if (!W.sc || W.recreate) {
            int k = sc_make();
            if (k <= 0) { pthread_mutex_unlock(&g_sc_mu); return k; }
        }
        uint64_t t0 = plat_now_ns();
        r = vkAcquireNextImageKHR(hvk.d, W.sc, ACQUIRE_NS, W.acq[f], VK_NULL_HANDLE, &idx);
        uint64_t t1 = plat_now_ns();
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            if (r == VK_SUBOPTIMAL_KHR) W.recreate = 1;   // presented as it is, then made again
            if (t1 - t0 > 2000000ull && W.mode == VK_PRESENT_MODE_FIFO_KHR && !hvk.dev.x.present_wait && !DISPLAY_TIMES) anchor(t1, 1);   // FIFO released an image at a refresh
            break;
        }
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { W.recreate = 1; continue; }
        pthread_mutex_unlock(&g_sc_mu);
        if (r == VK_TIMEOUT || r == VK_NOT_READY) { HVK_RED_ONCE("window presenter: no swapchain image within %llu ms (a window the compositor does not show?): frames are dropped meanwhile", ACQUIRE_NS / 1000000ull); return 0; }
        hostvk_log("[RED] [hostvk] window presenter: vkAcquireNextImageKHR failed (VkResult %d): frames are no longer shown", r);
        return -1;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { pthread_mutex_unlock(&g_sc_mu); return 0; }   // out of date twice
    VkImage dst = W.img[idx];
    int32_t rect[4]; int how = scale_plan(s, idx, rect);
    int linear = how == SCALE_BLIT && !(rect[2] == (int32_t)s->w && rect[3] == (int32_t)s->h);   // one to one: nearest, the bytes copied as they are
    if (linear) {
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(hvk.dev.phys, s->fmt, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) { linear = 0; HVK_RED_ONCE("window presenter: format %d cannot be filtered linearly: the picture is scaled nearest", s->fmt); }
    }
    VkCommandBuffer cb = W.fcb[f];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(cb, &bi);
    const VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // After the snapshot's copy (earlier on the queue) and the acquire (the semaphore wait, at every stage): the image to TRANSFER_DST, its old content discarded
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, dst, whole};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 1, &ib};
    vkCmdPipelineBarrier2(cb, &di);
    if (rect[2] != (int32_t)W.ext.width || rect[3] != (int32_t)W.ext.height) {   // the letterbox bars: black
        VkClearColorValue black = {{0, 0, 0, 1}};
        vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);
        VkMemoryBarrier2 ww = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
        VkDependencyInfo d2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &ww, 0, NULL, 0, NULL};
        vkCmdPipelineBarrier2(cb, &d2);
    }
    // The blit: the snapshot (its rows bottom up with the lower-left origin) into the picture's rectangle (bilinear; nearest at one to one and at an integer multiple); or FSR 1
    VkImageLayout last = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; VkPipelineStageFlags2 last_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; VkAccessFlags2 last_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (how == SCALE_FSR_DIRECT || how == SCALE_FSR_COPY) fsr_record(cb, f, s, dst, idx, rect, how == SCALE_FSR_DIRECT, &last, &last_stage, &last_access);
    else {
        VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, s->flip ? (int32_t)s->h : 0, 0}, {(int32_t)s->w, s->flip ? 0 : (int32_t)s->h, 1}},
                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{rect[0], rect[1], 0}, {rect[0] + rect[2], rect[1] + rect[3], 1}}};
        vkCmdBlitImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    int rb = 0;
    if (W.readback) {   // the tests: the picture as it goes to the presentation engine
        VkDeviceSize need = (VkDeviceSize)W.ext.width * W.ext.height * 4;
        if (need > W.rb_size) {
            tl_wait(W.last_val);
            if (W.rb) hvk_mem_buffer_free(W.rb, W.rb_mem);
            W.rb = VK_NULL_HANDLE; W.rb_mem = NULL; W.rb_map = NULL; W.rb_size = 0;
            if (!hvk_mem_buffer(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, HVK_MEM_HOST, &W.rb, &W.rb_mem, &W.rb_map)) W.rb_size = need;
        }
        rb = W.rb_size >= need;
    }
    if (rb) {
        VkImageMemoryBarrier2 tb = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, last_stage, last_access, VK_PIPELINE_STAGE_2_COPY_BIT,
                                    VK_ACCESS_2_TRANSFER_READ_BIT, last, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, dst, whole};
        VkDependencyInfo d3 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &tb};
        vkCmdPipelineBarrier2(cb, &d3);
        VkBufferImageCopy bc = {0, W.ext.width, W.ext.height, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W.ext.width, W.ext.height, 1}};
        vkCmdCopyImageToBuffer(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, W.rb, 1, &bc);
        VkMemoryBarrier2 hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
        VkDependencyInfo d4 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &hb, 0, NULL, 0, NULL};
        vkCmdPipelineBarrier2(cb, &d4);
        last = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; last_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; last_access = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
    }
    VkImageMemoryBarrier2 pb = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, last_stage, last_access,
                                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, last, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, dst, whole};
    VkDependencyInfo d5 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &pb};
    vkCmdPipelineBarrier2(cb, &d5);
    vkEndCommandBuffer(cb);
    VkSemaphoreSubmitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, W.acq[f], 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
    VkResult rs;
    uint64_t v = tl_submit(cb, &wait, 1, W.done[idx], &rs);
    if (!v) { pthread_mutex_unlock(&g_sc_mu); hostvk_log("[RED] [hostvk] window presenter: the blit's (or the upscaler's) submission failed (VkResult %d): frames are no longer shown", rs); return -1; }
    W.fval[f] = v; W.frame++;
    s->read = v;
    // The present, with its id (present wait) or its presentation time request (display timing: the time it is shown, looked up later)
    VkPresentIdKHR pid = {VK_STRUCTURE_TYPE_PRESENT_ID_KHR, NULL, 1, &s->n};
    VkPresentTimeGOOGLE ptime = {(uint32_t)s->n, 0};
    VkPresentTimesInfoGOOGLE pti = {VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE, NULL, 1, &ptime};
    const void *chain = hvk.dev.x.present_wait ? (const void *)&pid : hvk.dev.x.display_timing ? (const void *)&pti : NULL;
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, chain, 1, &W.done[idx], 1, &W.sc, &idx, NULL};
    pthread_mutex_lock(&hvk.queue_mu);
    VkResult pr = vkQueuePresentKHR(hvk.dev.queue, &pi);
    pthread_mutex_unlock(&hvk.queue_mu);
    if (pr == VK_SUBOPTIMAL_KHR || pr == VK_ERROR_OUT_OF_DATE_KHR) W.recreate = 1;
    else if (pr != VK_SUCCESS) { pthread_mutex_unlock(&g_sc_mu); hostvk_log("[RED] [hostvk] window presenter: vkQueuePresentKHR failed (VkResult %d): frames are no longer shown", pr); return -1; }
    // When it reaches the screen: the waiter's queue (present wait), or the past timings so far (display timing; the time base is CLOCK_MONOTONIC, plat_now_ns's on Linux)
    if (hvk.dev.x.present_wait) {
        pthread_mutex_lock(&g_mu);
        if (W.pw_wr - W.pw_rd < PW_RING) { W.pw_q[W.pw_wr % PW_RING] = s->n; W.pw_qgen[W.pw_wr % PW_RING] = W.sc_gen; W.pw_wr++; pthread_cond_broadcast(&g_cv); }
        pthread_mutex_unlock(&g_mu);
    } else if (hvk.dev.x.display_timing) {
        VkPastPresentationTimingGOOGLE pt[16]; uint32_t k = 16;
        if (vkGetPastPresentationTimingGOOGLE(hvk.d, W.sc, &k, pt) >= 0 && k) {
#if !defined(_WIN32)
            pthread_mutex_lock(&g_mu);
            for (uint32_t i = 0; i < k; i++) ring_put(pt[i].presentID, (double)pt[i].actualPresentTime / 1e9);
            pthread_mutex_unlock(&g_mu);
            anchor(pt[k - 1].actualPresentTime, 2);
#else   // open: no Windows driver documents the time base of actualPresentTime (plat_now_ns is QueryPerformanceCounter's), so the times are not used (DISPLAY_TIMES)
            HVK_RED_ONCE("window presenter: VK_GOOGLE_display_timing's present times have no known time base on Windows: not used; the FIFO acquire anchors the vsync clock "
                         "instead (coarser than the present times) and presented frames have no times");
#endif
        }
    }
    pthread_mutex_lock(&v_mu); int kind = V.kind; pthread_mutex_unlock(&v_mu);
    pthread_mutex_lock(&g_mu);
    W.shown++;
    W.last = (hvk_win_info){W.shown, s->copied, W.swapchains, W.dropped, W.ext.width, W.ext.height, W.nimg, W.fmt, W.mode, {rect[0], rect[1], rect[2], rect[3]},
                            s->w, s->h, s->flip, linear, kind, how};
    W.last_val = v;
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_unlock(&g_sc_mu);
    return 1;
}
// The waiter (VK_KHR_present_wait): when each present reached the screen. vkWaitForPresentKHR needs the swapchain's host access to itself, so it is polled under g_sc_mu, without
// waiting there (a present or an acquire is never held up), every half millisecond: that is the time's precision
static void *wait_main(void *arg) {
    (void)arg;
    plat_thread_set_name("present wait");
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!W.stop && W.pw_rd == W.pw_wr) pthread_cond_wait(&g_cv, &g_mu);
        if (W.stop) { pthread_mutex_unlock(&g_mu); break; }
        uint64_t id = W.pw_q[W.pw_rd % PW_RING], gen = W.pw_qgen[W.pw_rd % PW_RING];
        pthread_mutex_unlock(&g_mu);
        VkResult r = VK_TIMEOUT;
        pthread_mutex_lock(&g_sc_mu);
        if (gen == W.sc_gen && W.sc) r = vkWaitForPresentKHR(hvk.d, W.sc, id, 0);
        else r = VK_ERROR_OUT_OF_DATE_KHR;   // its swapchain is gone: not known
        pthread_mutex_unlock(&g_sc_mu);
        if (r == VK_TIMEOUT) { plat_sleep_ns(500000); continue; }
        uint64_t t = plat_now_ns();
        pthread_mutex_lock(&g_mu);
        if (r == VK_SUCCESS) ring_put(id, (double)t / 1e9);
        W.pw_rd++;
        pthread_mutex_unlock(&g_mu);
        if (r == VK_SUCCESS) anchor(t, 3);
    }
    return NULL;
}
static void win_fail(void) {   // the window presenter stops for good: later presents go to the headless presenter (vk_backend.c), queued snapshots are dropped
    pthread_mutex_lock(&g_mu);
    W.failed = 1; W.on = 0;
    while (W.rd < W.wr) { ring_put(W.snap[W.rd % SNAP_N].n, -1); W.rd++; W.dropped++; }
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&v_mu); V.on = 0; pthread_cond_broadcast(&v_cv); pthread_mutex_unlock(&v_mu);
}
static void *present_main(void *arg) {
    (void)arg;
    plat_thread_set_name("present");
    plat_qos_set_self(PLAT_QOS_USER_INTERACTIVE);
    // The surface: the front end makes it on its main thread (front_sdl.c stops drawing its progress screen into the window first)
    void *surf = NULL;
    if (!g_make_surface || g_make_surface((void *)hvk.dev.instance, &surf) || !surf) { hostvk_log("[RED] [hostvk] window presenter: the front end made no Vulkan surface: frames are not shown in the window"); win_fail(); return NULL; }
    W.surface = (VkSurfaceKHR)surf;
    VkBool32 sup = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(hvk.dev.phys, hvk.dev.queue_family, W.surface, &sup) || !sup) {
        hostvk_log("[RED] [hostvk] window presenter: queue family %u of '%s' cannot present to the window (a GPU that drives no screen?): frames are not shown in the window", hvk.dev.queue_family, hvk.dev.props.deviceName);
        win_fail(); return NULL;
    }
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, hvk.dev.queue_family};
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, VK_COMMAND_BUFFER_LEVEL_PRIMARY, FRAME_N};
    int ok = !vkCreateCommandPool(hvk.d, &pci, NULL, &W.fpool);
    if (ok) { ai.commandPool = W.fpool; ok = !vkAllocateCommandBuffers(hvk.d, &ai, W.fcb); }
    for (int i = 0; ok && i < FRAME_N; i++) ok = !vkCreateSemaphore(hvk.d, &si, NULL, &W.acq[i]);
    if (!ok) { hostvk_log("[RED] [hostvk] window presenter: its command buffers or semaphores could not be made: frames are not shown in the window"); win_fail(); return NULL; }
    pthread_mutex_lock(&g_sc_mu);
    int k = sc_make();
    pthread_mutex_unlock(&g_sc_mu);
    if (k < 0) { win_fail(); return NULL; }
    // On: the vsync source starts with the first swapchain, as the display link starts with Metal's layer
    pthread_mutex_lock(&g_mu); W.on = 1; pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&v_mu); V.on = 1; pthread_mutex_unlock(&v_mu);
    if (!pthread_create(&W.tick_th, NULL, tick_main, NULL)) W.tick_on = 1;
    else { HVK_RED_ONCE("window presenter: the vsync thread could not start: the game paces on its own clock"); pthread_mutex_lock(&v_mu); V.on = 0; pthread_mutex_unlock(&v_mu); }
    if (hvk.dev.x.present_wait && !pthread_create(&W.wait_th, NULL, wait_main, NULL)) W.wait_on = 1;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!W.stop && W.rd == W.wr) pthread_cond_wait(&g_cv, &g_mu);
        if (W.stop) { pthread_mutex_unlock(&g_mu); break; }
        snap_t *s = &W.snap[W.rd % SNAP_N];
        pthread_mutex_unlock(&g_mu);
        int r = show(s);
        if (r < 0) { win_fail(); break; }
        if (!r) { drop_snapshot(s); continue; }
        pthread_mutex_lock(&g_mu); W.rd++; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}

// ---------- The game's side ----------
int hvk_win_wanted(void) {   // once per device bring-up (vk_backend.c)
    const char *e = getenv("DM_VK_HEADLESS");
    unsigned n = 0;
    if (!g_make_surface) return 0;   // no front end registered (the GPU tests, a host without a window)
    if (e && *e && strcmp(e, "0")) { hostvk_log("[hostvk] DM_VK_HEADLESS=%s: frames go to the headless presenter, not to the window", e); return 0; }
    return front_vk_instance_extensions(NULL, &n) > 0;   // else no Vulkan window (none opened, or SDL's video driver has no Vulkan: front_sdl.c says which)
}
void hvk_win_device_up(int with_surface) { W.wanted = with_surface; W.device_ok = with_surface; }
int hvk_win_ready(void) { return W.wanted && W.device_ok && !W.failed; }
int hvk_win_on(void) { return W.on; }
int hvk_win_display_hz(void) { return W.on ? screen_hz() : 0; }
uint32_t hvk_win_qos(void) { return W.th_on ? PLAT_QOS_USER_INTERACTIVE : PLAT_QOS_UNSPECIFIED; }
double hvk_win_presented_time(uint64_t n) {
    pthread_mutex_lock(&g_mu); pres_ent e = W.ring[n % PRES_RING]; pthread_mutex_unlock(&g_mu);
    return e.n == n ? e.t : 0;
}
static int start(void) {   // under g_mu: the timeline, the snapshots' pool, the present thread
    if (W.started) return W.tl != VK_NULL_HANDLE;
    W.started = 1;
    up_parse(getenv("DM_PRESENT_UPSCALE"), getenv("DM_PRESENT_SHARPEN"));   // the upscaler (design 7.4, decision D4), on the present thread
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti, 0};
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, hvk.dev.queue_family};
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vkCreateSemaphore(hvk.d, &si, NULL, &W.tl)) { W.tl = VK_NULL_HANDLE; return 0; }
    if (vkCreateCommandPool(hvk.d, &pci, NULL, &W.pool)) { W.pool = VK_NULL_HANDLE; return 0; }
    ai.commandPool = W.pool;
    for (int i = 0; i < SNAP_N; i++) if (vkAllocateCommandBuffers(hvk.d, &ai, &W.snap[i].cb)) return 0;
    if (pthread_create(&W.th, NULL, present_main, NULL)) return 0;
    W.th_on = 1;
    return 1;
}
uint64_t hvk_win_present(VkImage image, VkFormat fmt, VkImageLayout layout, uint32_t w, uint32_t h, const int crop[4], int flip, VkSemaphore wait, uint64_t wait_val) {
    uint32_t x = 0, y = 0, cw = w, ch = h;
    if (crop && crop[0] >= 0 && crop[1] >= 0 && crop[2] > 0 && crop[3] > 0 && (uint32_t)crop[0] + (uint32_t)crop[2] <= w && (uint32_t)crop[1] + (uint32_t)crop[3] <= h) {
        x = (uint32_t)crop[0]; y = (uint32_t)crop[1]; cw = (uint32_t)crop[2]; ch = (uint32_t)crop[3];
    }
    pthread_mutex_lock(&g_mu);
    if (!start()) { pthread_mutex_unlock(&g_mu); win_fail(); hostvk_log("[RED] [hostvk] window presenter: its timeline, command pool or thread could not be made: frames go to the headless presenter"); return 0; }
    uint64_t n = ++W.present_n;
    // A free snapshot: the present thread has taken the oldest; a present thread stuck for SNAP_WAIT_NS (a hidden window, a surface gone) drops this frame instead of holding the game
    uint64_t limit = plat_now_ns() + SNAP_WAIT_NS;
    while (!W.failed && W.wr - W.rd >= SNAP_N) {
        uint64_t now = plat_now_ns();
        if (now >= limit) {
            W.dropped++; ring_put(n, -1);
            pthread_mutex_unlock(&g_mu);
            HVK_RED_ONCE("window presenter: the present thread took no frame for %llu ms: frames are dropped until it does", SNAP_WAIT_NS / 1000000ull);
            return 0;
        }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 10000000L; while (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    }
    if (W.failed) { pthread_mutex_unlock(&g_mu); return 0; }
    snap_t *s = &W.snap[W.wr % SNAP_N];
    VkFormat raw = raw_twin(fmt);
    tl_wait(s->copied);   // its command buffer is free
    if (s->img && (s->fmt != raw || s->w != cw || s->h != ch)) { tl_wait(s->read); snap_free(s); }   // another size or format: the blit (or FSR) that read it is done
    if (!s->img) {
        int sampled = (fmt_features(raw) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;   // FSR's EASU samples it (linear, clamp to edge)
        if (!image_make(raw, cw, ch, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0), &s->img, &s->mem)) {
            pthread_mutex_unlock(&g_mu);
            HVK_RED_ONCE("window presenter: no snapshot image of %ux%u in format %d: the frame is not shown", cw, ch, raw);
            return 0;
        }
        s->fmt = raw; s->w = cw; s->h = ch; s->sampled = sampled;
    }
    VkCommandBuffer cb = s->cb;
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(cb, &bi);
    const VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // After the producer's frame (earlier on the queue, or the timeline value waited for) and the previous blit from this snapshot: the snapshot to TRANSFER_DST, discarded
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, s->img, whole};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 1, &ib};
    vkCmdPipelineBarrier2(cb, &di);
    VkImageCopy c = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {(int32_t)x, (int32_t)y, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {cw, ch, 1}};
    vkCmdCopyImage(cb, image, layout, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    // The snapshot for the blit; the frame's image for whatever follows on the queue (the producer drawing into it again)
    VkMemoryBarrier2 after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkImageMemoryBarrier2 ob = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                                s->img, whole};
    VkDependencyInfo d2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &after, 0, NULL, 1, &ob};
    vkCmdPipelineBarrier2(cb, &d2);
    vkEndCommandBuffer(cb);
    VkSemaphoreSubmitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, wait, wait_val, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
    VkResult rs;
    uint64_t v = tl_submit(cb, wait ? &wi : NULL, wait ? 1u : 0u, VK_NULL_HANDLE, &rs);
    if (!v) { pthread_mutex_unlock(&g_mu); HVK_RED_ONCE("window presenter: the snapshot copy's submission failed (VkResult %d): the frame is not shown", rs); return 0; }
    s->copied = v; s->n = n; s->flip = flip;
    W.wr++; W.last_ticket = v;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return v;
}
void hvk_win_present_wait(uint64_t ticket) { tl_wait(ticket); }
void hvk_win_flush(void) { pthread_mutex_lock(&g_mu); uint64_t t = W.last_ticket; pthread_mutex_unlock(&g_mu); tl_wait(t); }
void hvk_win_shutdown(void) {
    pthread_mutex_lock(&g_mu); W.stop = 1; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&v_mu); V.stop = 1; V.on = 0; pthread_cond_broadcast(&v_cv); pthread_mutex_unlock(&v_mu);
    if (W.th_on) { pthread_join(W.th, NULL); W.th_on = 0; }
    if (W.tick_on) { pthread_join(W.tick_th, NULL); W.tick_on = 0; }
    if (W.wait_on) { pthread_join(W.wait_th, NULL); W.wait_on = 0; }
    if (!hvk.d) return;
    tl_wait(tl_last());
    pthread_mutex_lock(&g_sc_mu); sc_destroy(); pthread_mutex_unlock(&g_sc_mu);
    for (int i = 0; i < SNAP_N; i++) snap_free(&W.snap[i]);
    fsr_free();
    for (int i = 0; i < FRAME_N; i++) if (W.acq[i]) { vkDestroySemaphore(hvk.d, W.acq[i], NULL); W.acq[i] = VK_NULL_HANDLE; }
    if (W.rb) hvk_mem_buffer_free(W.rb, W.rb_mem);
    if (W.fpool) vkDestroyCommandPool(hvk.d, W.fpool, NULL);
    if (W.pool) vkDestroyCommandPool(hvk.d, W.pool, NULL);
    if (W.tl) vkDestroySemaphore(hvk.d, W.tl, NULL);
    if (W.surface) vkDestroySurfaceKHR(hvk.dev.instance, W.surface, NULL);   // the front end made it on this instance (SDL_Vulkan_DestroySurface is this call)
    memset(&W, 0, sizeof W);   // a later bring-up (the tests) starts clean; the locks and the registered window stay
    pthread_mutex_lock(&v_mu); memset(&V, 0, sizeof V); pthread_mutex_unlock(&v_mu);
}

// ---------- Test hooks ----------
void hvk_win_readback(int on) { pthread_mutex_lock(&g_sc_mu); W.readback = on; pthread_mutex_unlock(&g_sc_mu); }
void hvk_win_upscale(const char *mode, const char *sharpen, int no_direct) {
    pthread_mutex_lock(&g_sc_mu);
    up_parse(mode, sharpen); W.up_no_direct = no_direct;
    W.recreate = 1;   // the swapchain's storage usage follows (sc_make)
    pthread_mutex_unlock(&g_sc_mu);
}
int hvk_win_last(void *texels, size_t n, hvk_win_info *info) {
    // Every frame presented so far shown first: the present thread takes them after the game's call returned (at most 2 s here: a stuck presenter is reported as it is)
    uint64_t limit = plat_now_ns() + 2000000000ull;
    pthread_mutex_lock(&g_mu);
    while (W.rd < W.wr && !W.failed && plat_now_ns() < limit) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 10000000L; while (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    }
    uint64_t v = W.last_val; hvk_win_info i = W.last;
    pthread_mutex_unlock(&g_mu);
    if (!v) return 0;
    tl_wait(v);
    pthread_mutex_lock(&g_sc_mu);
    size_t bytes = (size_t)i.width * i.height * 4;
    if (texels && W.rb_map) memcpy(texels, W.rb_map, bytes < n ? bytes : n);
    pthread_mutex_unlock(&g_sc_mu);
    if (info) *info = i;
    return 1;
}
