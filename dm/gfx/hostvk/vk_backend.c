// The Vulkan backend's entry points (docs/design/vulkan-backend.md 1.5, phase P3): every function of dm/gfx/nvn/nvn_be.h and nvn_be_ext.h for the hosts without Metal, the hooks
// the frontend's state, handle and pool code calls in its backend (nvn_state.h, nvn_resolve.h, nvn_pools.h), and be_get_caps / be_guest_map / be_guest_unmap of design 1.5.
// The device comes up on the first call that needs it (as the Metal backend's mtl_init), through hostvk_init: DM_VK_DEVICE=<index> picks the physical device (design 10 item 4),
// DM_VK_VALIDATION=1 turns the Khronos validation layer on with synchronization validation (design 10 item 7), DM_VK_LEVEL=A|B|C|D makes the device as one of that level of design
// 3.6 would be (for tests and measurements of levels C and D on one machine; phase P5), DM_VK_MAX_ALLOCATIONS=<n> keeps the backend under n live device memory allocations
// instead of the device's maxMemoryAllocationCount (a test switch, so lavapipe stands for a driver with a low limit; step D2, vk_alloc.h). With a Vulkan window registered by
// the host program (vk_present.h), the device is made with VK_KHR_swapchain and frames go to the window presenter (vk_present.c, step P4a); otherwise, or with
// DM_VK_HEADLESS=1, to the headless presenter below.
// Fallback: when no Vulkan device meets the baseline (no loader, no driver, a device below design 9.1), every function takes the null backend's form instead (dm/gfx/nvn/nvn_null.c:
// one [RED] line per function on its first call, a GPU that has finished everything, events and syncs written at once, the pool table kept), so a host without Vulkan still runs
// its CPU side. The two forms live in this one library: a host binary links exactly one backend (dm/host/CMakeLists.txt: nxhost links hostvk; nvn_null.c stays the backend of the
// CMake targets that do not link hostvk, t_frontend).
// Declared here, with a red line and the phase that brings them: be_report's statistics lines (the per-frame statistics answer zero or what the backend counts), the DM_CTL
// diagnostics of the Metal backend (dm_gfx_shot_at and the texture and pass dumps, nvn_mtl.m). Presentation and the vsync source: vk_present.c in a window (step P4a), the
// headless presenter below without one.
#include "plat/plat.h"
#include "vk_backend.h"
#include "vk_pso.h"
#include "guest_vk.h"
#include "vk_present.h"
#include "../nvn/nvn_be.h"
#include "../nvn/nvn_pools.h"
#include "../nvn/nvn_replay.h"
#include "../nvn/nvn_resolve.h"
#include "../nvn/nvn_state.h"
#include "../nvn/nvn_tex.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hvk_t hvk = {.mu = PTHREAD_MUTEX_INITIALIZER, .tex_mu = PTHREAD_MUTEX_INITIALIZER, .queue_mu = PTHREAD_MUTEX_INITIALIZER};

// The other files' entry points (their headers stay internal to their concern)
void hvk_pool_add(const void *mem, uint64_t size, uint32_t nvn_flags);
void hvk_pool_del(const void *mem);
void hvk_pool_hook_use(nvn_pool *p);
void hvk_pool_hook_del(nvn_pool *p);
int hvk_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off);
void hvk_guest_unmap(void *mem, uint64_t size);
void hvk_tex_init(const tex_t *t);
void hvk_tex_release(tex_t *t);
void hvk_tex_cpu_written(const tex_t *t);
int hvk_tex_gpu_owned(uint64_t serial);
void hvk_reg_tex(int tid, const tex_t *t, const view_t *v, int image);
void hvk_reg_samp(int sid, const samp_par *p);
void hvk_sub_begin(void);
uint64_t hvk_sub_commit(void);
uint64_t hvk_last(void);
int hvk_wait(uint64_t fence, uint64_t timeout_ns);
void hvk_finish(void);
void hvk_draw(const p_draw *dw, const p_draw_idx *di, const uint8_t *rec, uint32_t size);
void hvk_dispatch(const p_dispatch *dp);
void hvk_clear_color(const p_clear_color *c);
void hvk_clear_ds(const p_clear_ds *c);
void hvk_clear_tex(const p_clear_tex *c);
void hvk_copy_b2b(uint64_t src, uint64_t dst, uint64_t n);
void hvk_update_ub(const void *data, uint64_t dst, uint64_t n);
void hvk_fill(uint64_t dst, uint64_t n, uint32_t value);
void hvk_copy_b2t(const p_copy_b2t *c, int to_buffer);
void hvk_copy_t2t(const p_copy_t2t *c);
void hvk_barrier(int32_t bits);
void hvk_signal_event(volatile uint32_t *val, uint32_t mode, uint32_t v);
void hvk_set_rt(const p_set_rt *r);
void hvk_report_counter(uint64_t type, uint64_t addr);
void hvk_reset_counter(uint64_t type);
void hvk_render_cond(const p_render_cond *rc);
void hvk_tf_state(const p_tf_state *b);
void hvk_tf_bind_note(void);

// ---------- Bring-up and shutdown ----------
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static char g_why[1024];   // why the device is not up (for the null forms' red line)
static _Atomic int g_tried;   // the bring-up ran (a function that needs the device was called)
static void up_once(void) {
    atomic_store(&g_tried, 1);
    hostvk_opts o = {-1, 0, 0};
    const char *e = getenv("DM_VK_DEVICE");
    if (e && *e) { char *end; long v = strtol(e, &end, 10); if (*end || v < 0) hostvk_log("[RED] [hostvk] DM_VK_DEVICE=%s is not a device index: the first device that meets the baseline is used", e); else o.device_index = (int)v; }
    e = getenv("DM_VK_VALIDATION");
    o.validation = e && *e && strcmp(e, "0") != 0;
    e = getenv("DM_VK_LEVEL");
    if (e && *e) { if ((e[0] >= 'A' && e[0] <= 'D') && !e[1]) o.force_level = e[0]; else hostvk_log("[RED] [hostvk] DM_VK_LEVEL=%s is not A, B, C or D: the device's level is used", e); }
    e = getenv("DM_VK_MAX_ALLOCATIONS");
    if (e && *e) {
        char *end; unsigned long v = strtoul(e, &end, 10);
        if (*end || !v || v > UINT32_MAX) hostvk_log("[RED] [hostvk] DM_VK_MAX_ALLOCATIONS=%s is not a count from 1: the device's maxMemoryAllocationCount is used", e);
        else o.max_allocations = (uint32_t)v;
    }
    o.want_surface = hvk_win_wanted();   // a Vulkan window: VK_KHR_swapchain and the surface's instance extensions (vk_present.c makes the surface at the first present)
    int rc = hostvk_init(&o, &hvk.dev, g_why, sizeof g_why);
    if (rc && o.want_surface) {   // a device without what a window needs (VK_KHR_swapchain, the surface's instance extensions): headless rather than none
        hostvk_log("[RED] [hostvk] no Vulkan device with a window's needs (%s): the device is made again without them, and frames go to the headless presenter", g_why);
        o.want_surface = 0;
        rc = hostvk_init(&o, &hvk.dev, g_why, sizeof g_why);
    }
    if (rc) { hostvk_log("[RED] [hostvk] no Vulkan device for the NVN backend (%s): the null backend's forms answer, nothing is drawn", g_why); return; }
    hvk_win_device_up(o.want_surface);
    hvk.d = hvk.dev.device; hvk.lim = hvk.dev.props.limits;
    hvk_alloc_init();   // the device memory allocator of decision D2 (vk_alloc.c): its block sizes, logged
    if (!hvk.dev.caps.buffer_device_address) hostvk_log("[RED] [hostvk] the device has no buffer device address: counter reports and conditional rendering are not written by the GPU");
    if (!hvk_commands_init()) { hostvk_log("[RED] [hostvk] the command layer could not be made: the null backend's forms answer"); hvk_alloc_shutdown(); hostvk_destroy(&hvk.dev); hvk.d = VK_NULL_HANDLE; return; }
    hvk.on = 1;
    hvk_cache_init(); hvk_pso_init();   // the pipeline cache on disk is read now; the prewarm starts at the first submission (vk_prewarm.c)
    hostvk_log("[hostvk] NVN backend on Vulkan: library %s%s", hvk_shader_dir(), o.validation ? ", validation on (DM_VK_VALIDATION)" : "");
}
int hvk_up(void) { pthread_once(&g_once, up_once); return hvk.on; }
// For tests and tools (dm/gfx/tests/vk): the device the backend runs on (its validation counters), NULL when it is not up; and a full shutdown, after which nothing may be called
const hostvk_dev *hvk_device(void);
const hostvk_dev *hvk_device(void) { return hvk_up() ? &hvk.dev : NULL; }
void hvk_shutdown(void);
static void pres_shutdown(void);
void hvk_shutdown(void) {
    if (!hvk.on) return;
    hvk_win_shutdown();   // the window presenter's threads stopped, its swapchain, snapshots and surface released (step P4a)
    pres_shutdown();   // the headless presenter's copies finished, its objects released (external producers, step P9a)
    hvk_commands_shutdown();
    hvk_pso_shutdown(); hvk_prewarm_shutdown(); hvk_cache_shutdown();   // the workers stopped, every pipeline released, the cache written a last time
    hvk_pipeline_shutdown(); hvk_shader_shutdown(); hvk_tex_shutdown(); hvk_mem_shutdown();
    hostvk_destroy(&hvk.dev); hvk.d = VK_NULL_HANDLE; hvk.on = 0;
}

// ---------- The null forms (nvn_null.c's, for a host without a device) ----------
static void null_reached(_Atomic int *once, const char *fn) {
    if (atomic_exchange(once, 1)) return;
    fprintf(stderr, "[dm] [RED] null graphics backend (no Vulkan device: %s): %s called, and no GPU work is done for it\n", g_why[0] ? g_why : "not brought up", fn);
}
#define NULLRED() do { static _Atomic int once_; null_reached(&once_, __func__); } while (0)
#define DECLARED(...) do { static _Atomic int once_; if (!atomic_exchange(&once_, 1)) hostvk_log("[RED] [hostvk] " __VA_ARGS__); } while (0)
static _Atomic uint64_t g_null_fence;

// ---------- Device ----------
void be_init(void) { if (!hvk_up()) NULLRED(); }
const be_caps *be_get_caps(void) {
    static be_caps c; static int made;
    if (!made && hvk_up()) {
        const hostvk_caps *h = &hvk.dev.caps;
        c.unified_memory = h->unified_memory; c.gpl = h->gpl; c.eds3_blend = h->eds3_blend; c.eds3_raster = h->eds3_raster; c.vertex_input_dynamic = h->vertex_input_dynamic;
        c.cond_render = h->cond_render; c.xfb = h->xfb; c.geometry = h->geometry; c.tessellation = h->tessellation; c.astc_ldr = h->astc_ldr; c.logic_op = h->logic_op;
        c.max_samples = h->max_samples; c.max_point_size = h->max_point_size; c.max_vertex_attribs = h->max_vertex_attribs;
        made = 1;
    }
    if (!hvk.on) NULLRED();   // no device: every capability 0
    return &c;
}

// ---------- Memory pools ----------
static pthread_mutex_t g_null_mu = PTHREAD_MUTEX_INITIALIZER;   // the pool table's lock in the null form
void be_pool_add(const void *mem, uint64_t size, uint32_t nvn_flags) {
    if (hvk_up()) { hvk_pool_add(mem, size, nvn_flags); return; }
    NULLRED(); pthread_mutex_lock(&g_null_mu); nvn_pool_add(mem, size, nvn_flags, (uintptr_t)mem, NULL, (uint64_t)(uintptr_t)mem); pthread_mutex_unlock(&g_null_mu);
}
void be_pool_del(const void *mem) {
    if (hvk.on) { hvk_pool_del(mem); return; }
    NULLRED(); pthread_mutex_lock(&g_null_mu); nvn_pool_del(mem); pthread_mutex_unlock(&g_null_mu);
}
void be_alias_drop_pool(const void *pool) { (void)pool; }   // the Metal backend's alias table (sparse views of a pool): the Vulkan imports have none to drop
int be_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    if (hvk.on) return hvk_addr(addr, len, buf, off);
    NULLRED();
    const nvn_pool_ref *c = nvn_pool_cached(addr, len);
    if (c) { *buf = c->backend; *off = addr - c->base; return 1; }
    int ok = 0; nvn_pool_ref r;
    pthread_mutex_lock(&g_null_mu);
    if (nvn_pool_lookup(addr, len, &r)) { *buf = r.backend; *off = addr - r.base; ok = 1; }
    pthread_mutex_unlock(&g_null_mu);
    return ok;
}
// Guest memory committed / about to be decommitted (design 1.5, 1.6; hle_svc.c's memory SVCs and the x86-64 engine's arena call them): the committed ranges are recorded from the
// first call, device or not (a game sizes its heap before it makes an NVN device), for the imports pools share where the allocation count nears the device's limit (vk_memory.c,
// step D2); a decommit under an import releases or moves it first. Before the device's bring-up was asked for (the host program's first guest memory SVCs) nothing is missing: the
// null forms' red line is for a device that could not be made
void be_guest_map(void *mem, uint64_t size) { hvk_region_map(mem, size, 1); if (!hvk.on && atomic_load(&g_tried)) NULLRED(); }
void be_guest_unmap(void *mem, uint64_t size) {
    if (hvk.on) { hvk_guest_unmap(mem, size); return; }
    hvk_region_map(mem, size, 0);
    if (atomic_load(&g_tried)) NULLRED();
}
void nvn_pool_hook_use(nvn_pool *p) { if (hvk.on) hvk_pool_hook_use(p); }   // nothing to make resident
void nvn_pool_hook_del(nvn_pool *p) { if (hvk.on) hvk_pool_hook_del(p); }   // the import goes when the GPU is done with it

// ---------- Object hooks ----------
void be_tex_init(const tex_t *t) { if (hvk_up()) hvk_tex_init(t); else NULLRED(); }
void be_tex_release(tex_t *t) { if (hvk.on) hvk_tex_release(t); else NULLRED(); }
void be_tex_cpu_written(const tex_t *t) { if (hvk_up()) hvk_tex_cpu_written(t); else NULLRED(); }
int be_tex_gpu_owned(uint64_t serial) { if (hvk.on) return hvk_tex_gpu_owned(serial); NULLRED(); return 0; }
void be_reg_tex(int tid, const tex_t *t, const view_t *v) { if (hvk_up()) hvk_reg_tex(tid, t, v, 0); else NULLRED(); }
void be_reg_image(int tid, const tex_t *t, const view_t *v) {   // RegisterImage declares image use (the Metal backend's mtl_reg_image): the texture gets storage use when it is made
    if (!hvk_up()) { NULLRED(); return; }
    if (!t) { hvk_reg_tex(tid, NULL, v, 1); return; }
    tex_t image = *t; image.d.flags |= NVN_TEXTURE_FLAGS_IMAGE;
    hvk_reg_tex(tid, &image, v, 1);
}
void be_reg_samp(int sid, const samp_par *sp) { if (hvk_up()) hvk_reg_samp(sid, sp); else NULLRED(); }
void be_set_tex_pool_n(int n) { atomic_store(&hvk.tex_pool_n, n); }
void be_prog_release(prog_t *p) { if (hvk.on) { if (p && p->serial) hvk_prog_drop(p->serial); } else NULLRED(); }
void be_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    if (hvk_up()) { hvk_tex_read(t, level, layer0, r, dst, row, img); return; }
    NULLRED();   // no texture holds GPU content: the texels read are zero (only the region's rows are written, each image's at its stride)
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb || !dst || r->width <= 0 || r->height <= 0) return;
    size_t wb = (size_t)((r->width + bw - 1) / bw) * (size_t)bpb, hb = (size_t)((r->height + bh - 1) / bh), nz = r->depth > 0 ? (size_t)r->depth : 1;
    for (size_t z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memset(dst + z * img + y * row, 0, wb);
}
void be_xv_window_tex(uint64_t serial) { (void)serial; }   // the Metal backend's cross-check of window textures (a diagnostic): none here
void be_set_depth_zero(int on) { atomic_store(&hvk.depth_zero, on != 0); }
void be_set_queue_origin_lower_left(int on) { hvk.queue_origin_ll = on != 0; }
void be_set_origin_lower_left(int on) { atomic_store(&hvk.origin_ll, on != 0); }

// ---------- Submission and sync (hle_nvn.c holds its submission lock) ----------
void be_queue_activate(uint64_t serial) { (void)nvn_state_queue_activate(serial); if (!hvk_up()) NULLRED(); }   // the frontend's per-queue state, as the Metal backend switches it
void be_queue_forget(uint64_t serial) { (void)nvn_state_queue_forget(serial); if (!hvk.on) NULLRED(); }
void be_sub_begin(void) { if (hvk_up()) hvk_sub_begin(); else NULLRED(); }
void be_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }   // the Metal backend's store look-ahead index: not used on Vulkan (design 1.5)
void be_exec_rest(const uint64_t *h, int n) { hvk_predict_rest_handles(h, n); }   // the handles still to come in the submission: the look-ahead goes on into them (vk_predict.c)
void be_exec(const uint8_t *handle) { if (!hvk.on) NULLRED(); nvn_replay_exec(handle); }   // the frontend's decoder hands every record to the functions below
uint64_t be_sub_commit(void) { if (hvk.on) return hvk_sub_commit(); NULLRED(); return atomic_fetch_add(&g_null_fence, 1) + 1; }
uint64_t be_last_fence(void) { if (hvk.on) return hvk_last(); NULLRED(); return atomic_load(&g_null_fence); }
int be_wait(uint64_t fence, uint64_t timeout_ns) { if (hvk.on) return hvk_wait(fence, timeout_ns); NULLRED(); (void)fence; (void)timeout_ns; return 1; }
// An acquire sync (fenced 2, by WindowAcquireTexture): the window's texture is free once the commits before the acquire completed; headless, that is the fence itself
int be_wait_acq(uint64_t fence, uint64_t timeout_ns) { if (hvk.on) return hvk_wait(fence, timeout_ns); NULLRED(); (void)fence; (void)timeout_ns; return 1; }
void be_finish(void) { if (hvk.on) hvk_finish(); else NULLRED(); }
void be_drain(void) { if (hvk.on) hvk_finish(); else NULLRED(); }

// ---------- Presentation and pacing (design 7; the window presenter and the vsync source are vk_present.c's, step P4a) ----------
// A present goes to the window presenter while it is on or about to start (hvk_win_ready), else to the headless presenter below. Their tickets are kept apart by WIN_TICKET, so a
// ticket waits on the presenter that gave it. Without a window the vsync functions answer "no source", and hle_nvn.c paces on its own clock, as it did before the presenter
#define WIN_TICKET (1ull << 62)
static uint64_t pres_headless(uint64_t serial, VkImage image, VkFormat format, VkImageLayout layout, uint32_t iw, uint32_t ih, void *ev, uint64_t ev_val, const int crop[4]);
static void pres_wait(uint64_t v);
static void pres_flush(void);
uint64_t be_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)period_s;   // the window presenter's FIFO and the acquire's vsync wait pace the frames (hle_nvn.c); the headless one paces nothing
    if (!hvk.on) { NULLRED(); return 0; }
    // The window's texture as the GPU left it: a texture no submission wrote (its content the CPU's) would need an upload, which only a submission records (vk_texture.c)
    if (!t || !hvk_tex_gpu_owned(t->serial)) { HVK_RED_ONCE("present of a texture no submission wrote (first ser=%llu): not shown", (unsigned long long)(t ? t->serial : 0)); return 0; }
    hvk_tex *x = hvk_tex_get(t);
    VkFormat fmt = VK_FORMAT_UNDEFINED; uint32_t w = 0, h = 0; VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    if (!x || !hvk_tex_info(x, &fmt, NULL, &w, &h, NULL, NULL, NULL, &samples, NULL) || samples != VK_SAMPLE_COUNT_1_BIT) {
        HVK_RED_ONCE("present of texture ser=%llu, which has no single-sampled image: not shown", (unsigned long long)t->serial); return 0;
    }
    int flip = atomic_load(&hvk.origin_ll);   // NVN's lower-left window origin: the image is stored bottom row first
    if (hvk_win_ready()) { uint64_t v = hvk_win_present(hvk_tex_image(x), fmt, VK_IMAGE_LAYOUT_GENERAL, w, h, crop, flip, VK_NULL_HANDLE, 0); return v ? v | WIN_TICKET : 0; }
    return pres_headless(0, hvk_tex_image(x), fmt, VK_IMAGE_LAYOUT_GENERAL, w, h, NULL, 0, crop);   // NVN's images stay in GENERAL (vk_texture.c)
}
void be_present_flush(void) { if (!hvk.on) { NULLRED(); return; } hvk_win_flush(); pres_flush(); }
int be_has_window(void) { return hvk_win_on(); }
int be_display_hz(void) { return hvk_win_display_hz(); }
int be_vsync_wait_ticks(int ticks, uint64_t timeout_ns) { return hvk_vsync_wait_ticks(ticks, timeout_ns); }
int be_vsync_rate(void) { return hvk_vsync_rate(); }
int be_vsync_stalled(void) { return hvk_vsync_stalled(); }
int be_vsync_tick(uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms) { (void)n; (void)target; (void)lead_ms; (void)ticks; (void)gap_ms; return 0; }   // the display link's per-tick record: none here
double be_presented_time(uint64_t n) { return hvk_win_presented_time(n); }
int be_snap_on(void) { return 0; }   // the window presenter's snapshots free the game's texture at once by construction (design 7.2): no ring for hle_nvn.c to manage
uint64_t be_snap_acquire_wait(void) { return 0; }
void be_present_wait(uint64_t ticket) { if (ticket & WIN_TICKET) hvk_win_present_wait(ticket & ~WIN_TICKET); else pres_wait(ticket); }
int be_fps_target(void) {
    static int env = -2;
    if (env == -2) { const char *e = getenv("DM_FPS"); env = e ? atoi(e) : -1; if (env != 30 && env != 60) env = -1; }
    extern int nvn_game_fps(void) __attribute__((weak));
    return env > 0 ? env : nvn_game_fps && nvn_game_fps() == 30 ? 30 : 60;   // the Metal backend's rule (mtl_fps_target)
}
int be_vsync_active(void) { return hvk_vsync_active(); }
int be_vsync_wait(int vsyncs, uint64_t timeout_ns) { return hvk_vsync_wait(vsyncs, timeout_ns); }
// The Metal backend's DM_CTL diagnostics (hle_hid.c "shot", "texlist", "texraw", "texdump", "passdump"; nvn_mtl.m): not on Vulkan yet, each a red line on its first call. The window
// presenter's readback (vk_present.h, for the tests) is where a screenshot would come from
void dm_gfx_shot_at(long k, const char *path);
void dm_gfx_texlist(int target);
void dm_gfx_texraw(uint64_t serial, const char *path);
void dm_gfx_texdump_next(const char *path);
void dm_gfx_passdump_next(long n);
void dm_gfx_shot_at(long k, const char *path) { (void)k; DECLARED("DM_CTL shot: screenshots are the Metal backend's (nvn_mtl.m), not written on Vulkan yet: %s not saved", path); }
void dm_gfx_texlist(int target) { (void)target; DECLARED("DM_CTL texlist: the Metal backend's texture list (nvn_mtl.m), not on Vulkan yet"); }
void dm_gfx_texraw(uint64_t serial, const char *path) { (void)serial; (void)path; DECLARED("DM_CTL texraw: the Metal backend's raw texture dump (nvn_mtl.m), not on Vulkan yet"); }
void dm_gfx_texdump_next(const char *path) { (void)path; DECLARED("DM_CTL texdump: the Metal backend's render-target dump (nvn_mtl.m), not on Vulkan yet"); }
void dm_gfx_passdump_next(long n) { (void)n; DECLARED("DM_CTL passdump: the Metal backend's pass dump (nvn_mtl.m), not on Vulkan yet"); }
// ---------- External producers (the guest Vulkan layer, guest_vk.c; step P9a) and the headless presenter ----------
// The shared device record is the backend's device layer (hostvk_dev). be_ext_tex registers an image (hvk_ext_image, guest_vk.h) under a serial; be_present_ext presents one, through
// the window presenter when it is on (vk_present.c), else through the headless presenter here, which also takes be_present's NVN frames without a window (DM_VK_HEADLESS=1, no
// Vulkan window): each present copies the frame's region (the layer crop, else the whole image) into a host-visible buffer, the picture a window would show (hvk_present_last
// reads it back). A present is one submission on the backend's queue: it waits for the producer's timeline value (ev, ev_val), copies between two full barriers, so the copy is
// ordered against everything submitted before and after it on the queue whatever semaphores the producer used, and signals the presenter's own timeline; that value is the ticket
// be_present_wait waits for. Up to PRES_N presents are in flight; the next one waits for the oldest. The period is not used: nothing paces a headless present (there is no vsync
// source without a window). With the lower-left window origin (be_set_origin_lower_left: the guest GL layer, guest_gl.c, stores its images bottom row first, as GL lays them out;
// NVN's lower-left window origin) the region's rows are copied bottom up, so the picture is upright, as the Metal backend's present flips it
void *be_native_device(void) { return hvk_up() ? (void *)&hvk.dev : NULL; }
typedef struct { VkImage image; VkFormat format; uint32_t w, h; } ext_ent;
enum { PRES_N = 4 };
static pthread_mutex_t g_ext_mu = PTHREAD_MUTEX_INITIALIZER;   // the registry and the presenter
static ext_ent *g_ext; static uint32_t g_n_ext, g_cap_ext;      // serial s names g_ext[s - 1]; a dropped one has no image
static VkCommandPool g_pres_pool; static VkCommandBuffer g_pres_cb[PRES_N]; static uint64_t g_pres_at[PRES_N];
static VkSemaphore g_pres_tl; static uint64_t g_pres_n;         // the presenter's timeline and the last value submitted
static VkBuffer g_screen; static hvk_mem *g_screen_mem; static void *g_screen_map; static VkDeviceSize g_screen_size;
static hvk_present_info g_last;
uint64_t be_ext_tex(void *tex) {
    if (!hvk_up()) { NULLRED(); return 0; }
    const hvk_ext_image *e = tex;
    if (!e || !e->image) return 0;
    pthread_mutex_lock(&g_ext_mu);
    if (g_n_ext == g_cap_ext) {
        uint32_t c = g_cap_ext ? 2 * g_cap_ext : 16; ext_ent *n = realloc(g_ext, c * sizeof *n);
        if (!n) { pthread_mutex_unlock(&g_ext_mu); HVK_RED_ONCE("external texture registry: out of memory"); return 0; }
        g_ext = n; g_cap_ext = c;
    }
    g_ext[g_n_ext++] = (ext_ent){e->image, e->format, e->width, e->height};
    uint64_t s = g_n_ext;
    pthread_mutex_unlock(&g_ext_mu);
    return s;
}
void hvk_ext_tex_drop(uint64_t serial) {
    pthread_mutex_lock(&g_ext_mu);
    if (serial && serial <= g_n_ext) g_ext[serial - 1].image = VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_ext_mu);
}
static void pres_wait(uint64_t v) {   // the present of ticket v has finished reading its image
    if (!v || !g_pres_tl) return;
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &g_pres_tl, &v};
    uint64_t t0 = plat_now_ns();
    while (vkWaitSemaphores(hvk.d, &wi, 1000000000ull) == VK_TIMEOUT)
        if (plat_now_ns() - t0 > 5000000000ull) HVK_RED_ONCE("a present has waited over 5 s for its frame (the producer's timeline value is never signalled?)");
}
static int pres_make(void) {   // under g_ext_mu
    if (g_pres_tl) return 1;
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, hvk.dev.queue_family};
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, VK_COMMAND_BUFFER_LEVEL_PRIMARY, PRES_N};
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti, 0};
    if (vkCreateCommandPool(hvk.d, &pi, NULL, &g_pres_pool)) { g_pres_pool = VK_NULL_HANDLE; return 0; }
    ai.commandPool = g_pres_pool;
    if (vkAllocateCommandBuffers(hvk.d, &ai, g_pres_cb) || vkCreateSemaphore(hvk.d, &si, NULL, &g_pres_tl)) {
        vkDestroyCommandPool(hvk.d, g_pres_pool, NULL); g_pres_pool = VK_NULL_HANDLE; g_pres_tl = VK_NULL_HANDLE; return 0;
    }
    return 1;
}
uint64_t be_present_ext(uint64_t serial, double period, void *ev, uint64_t ev_val, const int crop[4]) {
    (void)period;
    if (!hvk.on) { NULLRED(); return 0; }
    pthread_mutex_lock(&g_ext_mu);
    if (!serial || serial > g_n_ext || !g_ext[serial - 1].image) {
        pthread_mutex_unlock(&g_ext_mu);
        HVK_RED_ONCE("present of external texture %llu, which names no registered image: not shown", (unsigned long long)serial);
        return 0;
    }
    const ext_ent e = g_ext[serial - 1];
    pthread_mutex_unlock(&g_ext_mu);
    if (hvk_win_ready()) {
        uint64_t v = hvk_win_present(e.image, e.format, HVK_EXT_LAYOUT, e.w, e.h, crop, atomic_load(&hvk.origin_ll), (VkSemaphore)ev, ev_val);
        return v ? v | WIN_TICKET : 0;
    }
    return pres_headless(serial, e.image, e.format, HVK_EXT_LAYOUT, e.w, e.h, ev, ev_val, crop);
}
static int texel4(VkFormat f) {   // the headless presenter's buffer holds 4-byte texels
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT:
        return 1;
    default: return 0;
    }
}
static uint64_t pres_headless(uint64_t serial, VkImage image, VkFormat format, VkImageLayout layout, uint32_t iw, uint32_t ih, void *ev, uint64_t ev_val, const int crop[4]) {
    const ext_ent e = {image, format, iw, ih};
    if (!texel4(format)) { HVK_RED_ONCE("the headless presenter keeps 4-byte texels: a frame in format %d is not copied", format); return 0; }
    pthread_mutex_lock(&g_ext_mu);
    uint32_t x = 0, y = 0, w = e.w, h = e.h;
    if (crop && crop[0] >= 0 && crop[1] >= 0 && crop[2] > 0 && crop[3] > 0 && (uint32_t)crop[0] + (uint32_t)crop[2] <= e.w && (uint32_t)crop[1] + (uint32_t)crop[3] <= e.h) {
        x = (uint32_t)crop[0]; y = (uint32_t)crop[1]; w = (uint32_t)crop[2]; h = (uint32_t)crop[3];
    }
    VkDeviceSize need = (VkDeviceSize)w * h * 4;
    if (!pres_make()) { pthread_mutex_unlock(&g_ext_mu); HVK_RED_ONCE("the headless presenter's command pool or timeline could not be made: frames are not shown"); return 0; }
    if (need > g_screen_size) {   // a larger picture: the copies in flight write the old buffer
        pres_wait(g_pres_n);
        if (g_screen) hvk_mem_buffer_free(g_screen, g_screen_mem);
        g_screen = VK_NULL_HANDLE; g_screen_mem = NULL; g_screen_map = NULL; g_screen_size = 0;
        if (hvk_mem_buffer(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, HVK_MEM_HOST, &g_screen, &g_screen_mem, &g_screen_map)) {
            pthread_mutex_unlock(&g_ext_mu); HVK_RED_ONCE("the headless presenter's buffer of %llu bytes could not be made: frames are not shown", (unsigned long long)need); return 0;
        }
        g_screen_size = need;
    }
    int slot = (int)(g_pres_n % PRES_N);
    pres_wait(g_pres_at[slot]);   // its command buffer is free again
    VkCommandBuffer cb = g_pres_cb[slot];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(cb, &bi);
    hvk_full_barrier(cb);   // after the producer's frame and after the previous present's copy into the buffer
    if (atomic_load(&hvk.origin_ll)) {   // the image is stored bottom row first: one region per row, the last row first
        static VkBufferImageCopy *rows; static uint32_t nrows;
        if (h > nrows) { VkBufferImageCopy *n = realloc(rows, h * sizeof *n); if (n) { rows = n; nrows = h; } }
        uint32_t k = h <= nrows ? h : 0;
        for (uint32_t i = 0; i < k; i++) rows[i] = (VkBufferImageCopy){(VkDeviceSize)(h - 1 - i) * w * 4, w, 1, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {(int32_t)x, (int32_t)(y + i), 0}, {w, 1, 1}};
        if (k) vkCmdCopyImageToBuffer(cb, e.image, layout, g_screen, k, rows);
        else HVK_RED_ONCE("the headless presenter's row list could not be made: the frame is not shown");
    } else {
        VkBufferImageCopy r = {0, w, h, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {(int32_t)x, (int32_t)y, 0}, {w, h, 1}};
        vkCmdCopyImageToBuffer(cb, e.image, layout, g_screen, 1, &r);
    }
    VkMemoryBarrier2 mb[2] = {   // before whatever follows on the queue (the producer drawing into the image again) and before the host reads the buffer
        {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT},
        {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 2, mb, 0, NULL, 0, NULL};
    vkCmdPipelineBarrier2(cb, &di);
    vkEndCommandBuffer(cb);
    uint64_t v = g_pres_n + 1;
    VkSemaphoreSubmitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, (VkSemaphore)ev, ev_val, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
    VkSemaphoreSubmitInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, g_pres_tl, v, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
    VkCommandBufferSubmitInfo ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, cb, 0};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, ev ? 1u : 0u, &wait, 1, &ci, 1, &sig};
    pthread_mutex_lock(&hvk.queue_mu); VkResult rs = vkQueueSubmit2(hvk.dev.queue, 1, &si, VK_NULL_HANDLE); pthread_mutex_unlock(&hvk.queue_mu);
    if (rs) { pthread_mutex_unlock(&g_ext_mu); HVK_RED_ONCE("the headless present's submission failed (VkResult %d): the frame is not shown", rs); return 0; }
    g_pres_n = v; g_pres_at[slot] = v;
    g_last = (hvk_present_info){serial, v, g_last.frames + 1, x, y, w, h, e.format};
    pthread_mutex_unlock(&g_ext_mu);
    return v;
}
int hvk_present_last(void *texels, size_t n, hvk_present_info *info) {
    pthread_mutex_lock(&g_ext_mu);
    int ok = g_last.ticket != 0;
    if (ok) {
        pres_wait(g_last.ticket);
        size_t bytes = (size_t)g_last.width * g_last.height * 4;
        if (texels) memcpy(texels, g_screen_map, bytes < n ? bytes : n);
        if (info) *info = g_last;
    }
    pthread_mutex_unlock(&g_ext_mu);
    return ok;
}
static void pres_flush(void) { pthread_mutex_lock(&g_ext_mu); uint64_t v = g_pres_n; pthread_mutex_unlock(&g_ext_mu); pres_wait(v); }
static void pres_shutdown(void) {   // hvk_shutdown: every present finished, the presenter's objects released, the registry emptied
    pthread_mutex_lock(&g_ext_mu);
    pres_wait(g_pres_n);
    if (g_pres_tl) vkDestroySemaphore(hvk.d, g_pres_tl, NULL);
    if (g_pres_pool) vkDestroyCommandPool(hvk.d, g_pres_pool, NULL);
    if (g_screen) hvk_mem_buffer_free(g_screen, g_screen_mem);
    g_pres_tl = VK_NULL_HANDLE; g_pres_pool = VK_NULL_HANDLE; g_screen = VK_NULL_HANDLE; g_screen_mem = NULL; g_screen_map = NULL; g_screen_size = 0;
    g_pres_n = 0; memset(g_pres_at, 0, sizeof g_pres_at); memset(&g_last, 0, sizeof g_last);
    free(g_ext); g_ext = NULL; g_n_ext = g_cap_ext = 0;
    pthread_mutex_unlock(&g_ext_mu);
}

// ---------- Frames and measurements ----------
void be_set_frame(uint64_t f) { hvk.frame = f; }
double be_gpu_ms(uint64_t f) { (void)f; return -1; }   // no GPU time per frame yet (timestamps per submission are phase P4's pacing work)
int be_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; (void)t0; (void)t1; return 0; }
void be_ab_gpu(uint64_t f, double gpu_ms, double frame_ms) { (void)f; (void)gpu_ms; (void)frame_ms; }
void be_ub_present(double iv_ms) { (void)iv_ms; }

// ---------- Statistics: what the backend counts, zero for the Metal mechanisms it has not ----------
void be_mem_stat(unsigned long long frame, unsigned long long rss_mb) { (void)frame; (void)rss_mb; }
void be_stats(uint64_t *passes, uint64_t *draws) { *passes = atomic_load(&hvk.st_passes); *draws = atomic_load(&hvk.st_draws); }
void be_args_stats(uint64_t *enc, uint64_t *reuse) { uint64_t h = 0, m = 0, w = 0; if (hvk.on) hvk_desc_stats(&h, &m, &w); *enc = m; *reuse = h; }   // descriptor sets written / taken from the cache
void be_use_stats(uint64_t *call, uint64_t *skip) { *call = *skip = 0; }
void be_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip, uint64_t *copies, uint64_t *copy_bytes) { *resolve = *reuse = *set = *skip = *copies = *copy_bytes = 0; }
uint64_t be_ds_omit(void) { return 0; }
uint64_t be_drawable_ns_take(void) { return 0; }
void be_bw(uint64_t *load, uint64_t *store, uint64_t *full_rw_passes) { *load = *store = *full_rw_passes = 0; }
uint64_t be_udc_saved_bytes(void) { return 0; }
void be_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = atomic_load(&hvk.st_uploads); *reuploads = atomic_load(&hvk.st_reuploads); }
unsigned be_cpu_tex_count(void) { return 0; }
uint64_t be_pkg_edge_skips(void) { return 0; }
uint64_t be_pkg_miss(void) { return 0; }
uint64_t be_upload_ns_take(void) { return atomic_exchange(&hvk.st_upload_ns, 0); }
uint64_t be_upload_gpu_ns_take(void) { return 0; }
uint64_t be_upload_bytes(void) { return atomic_load(&hvk.st_upload_bytes); }
void be_create_stats(uint64_t *lib, uint64_t *pso) { *lib = atomic_load(&hvk.st_shaders); *pso = atomic_load(&hvk.st_pipelines); }
void be_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = 0; *pso_ns = atomic_load(&hvk.st_pipeline_ns); }   // pso_ns: the time draws waited for their pipelines
void be_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none) { *draws = *made = *none = 0; }
// The device-local heaps' budget and use (VK_EXT_memory_budget), in MiB; 0 when the device has not the extension
static int mem_budget(double *budget_mib, double *usage_mib) {
    *budget_mib = *usage_mib = 0;
    if (!hvk.on || !hvk.dev.x.memory_budget) return 0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &b};
    vkGetPhysicalDeviceMemoryProperties2(hvk.dev.phys, &p);
    for (uint32_t i = 0; i < p.memoryProperties.memoryHeapCount; i++)
        if (p.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) { *budget_mib += b.heapBudget[i] / 1048576.0; *usage_mib += b.heapUsage[i] / 1048576.0; }
    return 1;
}
// The backend's resource line (hle_nvn.c gives it 400 bytes, so it stays short): commands (step P8: passes split before feedback draws, indirect quads converted on the GPU,
// DrawTransformFeedback draws, draw-based copies), and texture memory (step P7, decision D1): every image, ASTC held compressed against ASTC decoded into RGBA8 (with what its blocks
// take), the uploads by path, and the device-local budget where VK_EXT_memory_budget gives it
void be_res_stats(char *out, size_t n) {
    if (!n) return;
    double mib = 1048576.0, bud, use;
    int k = snprintf(out, n, "vulkan: passes %llu draws %llu dispatches %llu pipelines %llu shaders %llu barriers %llu pass splits %llu GPU vertex copies %llu GPU quad indices %llu fb splits %llu ind quads %llu xfb draws %llu copy draws %llu; "
                     "textures %.1f MiB (ASTC %.1f native, %.1f decoded from %.1f of blocks), uploads GPU %llu CPU %llu, ASTC decodes %llu",
                     (unsigned long long)hvk.st_passes, (unsigned long long)hvk.st_draws, (unsigned long long)hvk.st_dispatches, (unsigned long long)hvk.st_pipelines,
                     (unsigned long long)hvk.st_shaders, (unsigned long long)hvk.st_barriers, (unsigned long long)hvk.st_pass_splits, (unsigned long long)hvk.st_vcopies_gpu,
                     (unsigned long long)hvk.st_index_gpu, (unsigned long long)atomic_load(&hvk.st_feedback_splits), (unsigned long long)atomic_load(&hvk.st_quad_ind),
                     (unsigned long long)atomic_load(&hvk.st_xfb_draws), (unsigned long long)atomic_load(&hvk.st_copy_draws), (double)atomic_load(&hvk.st_tex_bytes) / mib, (double)atomic_load(&hvk.st_astc_native_bytes) / mib,
                     (double)atomic_load(&hvk.st_astc_decoded_bytes) / mib, (double)atomic_load(&hvk.st_astc_block_bytes) / mib, (unsigned long long)atomic_load(&hvk.st_up_gpu),
                     (unsigned long long)atomic_load(&hvk.st_up_cpu), (unsigned long long)atomic_load(&hvk.st_astc_decodes));
    if (k > 0 && (size_t)k < n) {
        if (mem_budget(&bud, &use)) snprintf(out + k, n - (size_t)k, "; device-local %.0f of %.0f MiB", use, bud);
        else snprintf(out + k, n - (size_t)k, "; no memory budget");
    }
}
void be_barrier_stats(uint64_t *skip, uint64_t *full) { *skip = 0; *full = atomic_load(&hvk.st_barriers); }
uint32_t be_present_qos(void) { return hvk_win_qos(); }   // the window presenter's thread (vk_present.c); PLAT_QOS_UNSPECIFIED without one
uint64_t be_cut_count(void) { return 0; }
uint64_t be_copy_cut_count(void) { return 0; }
uint64_t be_blit_splits(void) { return atomic_load(&hvk.st_pass_splits); }
void be_ubd_stats(uint64_t *deferred, uint64_t *conflicts) { *deferred = *conflicts = 0; }
void be_ubp_stats(uint64_t *made, uint64_t *reused, uint64_t *refused) { *made = *reused = *refused = 0; }
void be_ubp_why(uint64_t w[5]) { memset(w, 0, 5 * sizeof *w); }
void be_lc_stats(uint64_t *taken, uint64_t *landed) { *taken = *landed = 0; }
void be_sm_stats(uint64_t *np, uint64_t *nc, uint64_t *nw) { *np = *nc = *nw = 0; }
void be_cshoist_stats(uint64_t *moved, uint64_t *depth, uint64_t *stops) { *moved = *depth = 0; memset(stops, 0, 8 * sizeof *stops); }   // stops: 8 counters, as hle_nvn.c reads them
// The pipeline line of hle_nvn.c's timing summary, the differences since the last call, in the shape of the Metal backend's (mtl_pso_line): what the submit thread spent (the
// look-ahead's walks as the Metal line's predictions), what every thread made, the background builds (the look-ahead's: queued, and at their draw ready, still being made or
// made by the draw; the speculative builds from the prewarm rows), and the draws that waited, per reason, with the longest wait of the window and the waits over the 2 ms budget
// (phase P5)
void be_pso_line(char *o, size_t n) {
    if (!n) return;
    if (!hvk.on) { snprintf(o, n, "vulkan: no device"); return; }
    hvk_pso_stats a; hvk_pso_stats_get(&a); hvk_prewarm_stats pw; hvk_prewarm_stats_get(&pw); hvk_cache_stats cs; hvk_cache_stats_get(&cs); hvk_predict_stats pr; hvk_predict_stats_get(&pr);
    enum { NV = 26, V = NV + 2 * HVK_W_N };
    uint64_t v[V] = {a.draw_ns, a.wait_ns[HVK_W_LIB], pw.speculative, a.lib[0] + a.lib[1] + a.lib[2] + a.lib[3], a.lib_ns[0] + a.lib_ns[1] + a.lib_ns[2] + a.lib_ns[3],
                     a.fast, a.fast_ns, a.opt, a.opt_ns, a.swapped, a.mono, a.mono_ns, pw.built, pw.failed, a.draw_made, a.over_budget, cs.created, cs.hits, a.opt_dropped, pw.stale,
                     pr.walks, pr.ns, a.pred_queued, a.pred_ready, a.pred_inflight, a.pred_made};
    for (int i = 0; i < HVK_W_N; i++) { v[NV + i] = a.wait_n[i]; v[NV + HVK_W_N + i] = a.wait_ns[i]; }
    static uint64_t p[V]; uint64_t d[V]; for (int i = 0; i < V; i++) d[i] = v[i] - p[i];
    size_t k = (size_t)snprintf(o, n, "on the submit thread pipelines %.1f ms, shader libraries %.1f ms, predictions %llu / %.1f ms; all threads (level %c): pipeline libraries %llu / %.1f ms, "
        "fast links %llu / %.1f ms, optimized links %llu / %.1f ms (swapped in %llu, dropped %llu), monolithic %llu / %.1f ms, driver cache hits %llu of %llu creations; "
        "background builds done %llu, failed %llu, made by the draw %llu, in flight %llu; look-ahead pipelines queued %llu (at their draw ready %llu, still being made %llu, "
        "made by the draw %llu), speculative from the rows %llu; draws that waited:",
        d[0] / 1e6, d[1] / 1e6, (unsigned long long)d[20], d[21] / 1e6, hvk_pso_level(), (unsigned long long)d[3], d[4] / 1e6, (unsigned long long)d[5], d[6] / 1e6, (unsigned long long)d[7], d[8] / 1e6,
        (unsigned long long)d[9], (unsigned long long)d[18], (unsigned long long)d[10], d[11] / 1e6, (unsigned long long)d[17], (unsigned long long)d[16], (unsigned long long)d[12],
        (unsigned long long)d[13], (unsigned long long)d[14], (unsigned long long)a.pending, (unsigned long long)d[22], (unsigned long long)d[23], (unsigned long long)d[24],
        (unsigned long long)d[25], (unsigned long long)d[2]);
    for (int i = 0; i < HVK_W_N; i++) if (d[NV + i] && k < n) k += (size_t)snprintf(o + k, n - k, " %s %llu / %.1f ms,", hvk_wait_why[i], (unsigned long long)d[NV + i], d[NV + HVK_W_N + i] / 1e6);
    if (k < n) snprintf(o + k, n - k, " longest %.2f ms, over 2 ms %llu", hvk_pso_take_window_max() / 1e6, (unsigned long long)d[15]);
    memcpy(p, v, sizeof p);
}
void be_alloc_stats(uint64_t *waits, uint64_t *grown, int *n) { *waits = *grown = 0; *n = 0; }

// ---------- Per operation (nvn_replay_exec, in record order) ----------
int be_list_begin(void) { return 0; }
int be_rec_pre(const uint8_t *rec, const op_hdr *h, int list) { (void)rec; (void)h; (void)list; return 0; }   // no look-ahead: every record reaches the decoder
void be_state_dirty(uint32_t mask) { (void)mask; }   // the draw reads S itself; pipelines and descriptor sets are cached by content
void be_end(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; (void)a; }
void be_signal_event(const uint8_t *rec, const op_hdr *h, const void *a) {
    (void)rec; (void)h;
    volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, (const uint8_t *)a + 8, 4); memcpy(&v, (const uint8_t *)a + 12, 4);
    if (hvk.on) { hvk_signal_event(val, m, v); return; }
    NULLRED();   // the event's value as the GPU would leave it, at once: the replay is the GPU here
    if (!val) return;
    dm_guest_write_prepare((void *)(uintptr_t)val, 4);
    if (m == NVN_EVENT_SIGNAL_MODE_ADD) *val += v; else *val = v;
}
void be_report_counter(const uint8_t *rec, const op_hdr *h, const void *a) {
    (void)rec; (void)h; uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, (const uint8_t *)a + 8, 8);
    if (hvk.on) hvk_report_counter(t, ad); else NULLRED();
}
void be_render_cond(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; if (hvk.on) hvk_render_cond(a); else NULLRED(); }
// The sync object is fenced with the submission that reaches it (the open one), as the Metal backend fences it
void be_fence_sync(const uint8_t *rec, const op_hdr *h, const void *a) {
    (void)rec; (void)h; sync_t *s; memcpy(&s, a, 8);
    if (!s) return;
    if (hvk.on) { uint64_t f = hvk_open_fence(); s->fence = f ? f : hvk_last() + 1; s->fenced = 1; return; }
    NULLRED(); s->fence = atomic_load(&g_null_fence) + 1; s->fenced = 1;
}
void be_reset_counter(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; uint64_t t; memcpy(&t, a, 8); if (hvk.on) hvk_reset_counter(t); else NULLRED(); }
void be_fill(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; const p_clear_buf *c = a; if (hvk.on) hvk_fill(c->addr, c->size, c->value); else NULLRED(); }
void be_copy_b2b(const uint8_t *rec, const op_hdr *h, const void *a) {
    (void)rec; (void)h; uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, (const uint8_t *)a + 8, 8); memcpy(&n, (const uint8_t *)a + 16, 8);
    if (hvk.on) hvk_copy_b2b(s, d, n); else NULLRED();
}
void be_update_ub(const uint8_t *rec, const op_hdr *h, const void *a) {
    (void)rec; (void)h; uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8);
    if (hvk.on) hvk_update_ub((const uint8_t *)a + 16, d, n); else NULLRED();
}
void be_set_rt(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; if (hvk.on) hvk_set_rt(a); else { NULLRED(); R.rt = *(const p_set_rt *)a; R.has_rt = 1; } }
void be_clear_color(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; if (hvk.on) hvk_clear_color(a); else NULLRED(); }
void be_clear_ds(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; if (hvk.on) hvk_clear_ds(a); else NULLRED(); }
void be_draw(const uint8_t *rec, const op_hdr *h, const void *a) { if (!hvk.on) { NULLRED(); return; } p_draw t; hvk_draw(draw_rec(a, h->size, &t), NULL, rec, h->size); }
void be_draw_idx(const uint8_t *rec, const op_hdr *h, const void *a) { if (!hvk.on) { NULLRED(); return; } p_draw_idx t; hvk_draw(NULL, draw_idx_rec(a, h->size, &t), rec, h->size); }
void be_bind_program(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; (void)a; }   // the program is resolved at the draw (S.prog, S.cprog)
void be_tf_bind(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; (void)a; if (hvk.on) hvk_tf_bind_note(); }   // an open capture segment ends before its buffers change (vk_commands.c)
void be_tf_state_note(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; (void)a; }
void be_tf_state(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; if (hvk.on) hvk_tf_state(a); else NULLRED(); }
void be_rast_discard(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; (void)a; }   // dynamic state: the next draw sets it from S
void be_copy_b2t(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; if (hvk.on) hvk_copy_b2t(a, 0); else NULLRED(); }
void be_copy_t2b(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; if (hvk.on) hvk_copy_b2t(a, 1); else NULLRED(); }
void be_copy_t2t(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; if (hvk.on) hvk_copy_t2t(a); else NULLRED(); }
void be_clear_tex(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; if (hvk.on) hvk_clear_tex(a); else NULLRED(); }
void be_dispatch(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; if (hvk.on) hvk_dispatch(a); else NULLRED(); }
void be_barrier(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; int32_t bits; memcpy(&bits, a, 4); if (hvk.on) hvk_barrier(bits); else NULLRED(); }

// ---------- The frontend's hooks into its backend ----------
uint64_t nvn_state_hook_compose(uint64_t old_serial, const prog_t *const bst[5]) { return hvk.on ? hvk_prog_compose(old_serial, bst) : nvn_new_serial(); }   // a combined program's serial
void nvn_state_hook_bind(uint32_t op, const p_bind_handles *b) { (void)op; nvn_bind_fresh(b); }   // deferred handles are read again before the bind applies (BindTexturesDeferred)
void nvn_state_hook_bind_bad(const void *rec, int n) { (void)rec; HVK_RED_ONCE("a texture bind record holds a handle count of %d (overwritten after it was recorded): skipped", n); }
void nvn_state_hook_img_range(const p_bind_handles *b) { HVK_RED_ONCE("an image bind of slots %d + %d past the 32 image slots: skipped", b->first, b->n); }
int nvn_resolve_hook_ubo_copy(uint64_t a, size_t n, void *out) { (void)a; (void)n; (void)out; return 0; }   // no copies of inline updates: the ledger has them (nvn_gw_pend_read)
void nvn_resolve_hook_ubo_stale(int stage, int reg, int off, int kind) {
    HVK_RED_ONCE("the uniform buffer holding bindless handles (stage %d slot %d +%#x) has GPU-side writes in this submission (%s): the current value is read", stage, reg, off,
                 kind == 2 ? "a buffer copy" : kind == 3 ? "a fill" : kind == 1 ? "an inline update covering part of it" : "more writes than the host tracks");
}
void nvn_resolve_hook_mem_read(uint64_t a, size_t n, void *out) {   // guest memory is host memory: read it where a pool holds it, as the GPU would
    void *b; uint64_t off;
    if (be_addr(a, n, &b, &off)) memcpy(out, (const void *)(uintptr_t)a, n);
}
