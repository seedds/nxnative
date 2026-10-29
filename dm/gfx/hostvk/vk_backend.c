// The Vulkan backend's entry points (docs/design/vulkan-backend.md 1.5, phase P3): every function of dm/gfx/nvn/nvn_be.h and nvn_be_ext.h for the hosts without Metal, the hooks
// the frontend's state, handle and pool code calls in its backend (nvn_state.h, nvn_resolve.h, nvn_pools.h), and be_get_caps / be_guest_map / be_guest_unmap of design 1.5.
// The device comes up on the first call that needs it (as the Metal backend's mtl_init), through hostvk_init: DM_VK_DEVICE=<index> picks the physical device (design 10 item 4),
// DM_VK_VALIDATION=1 turns the Khronos validation layer on with synchronization validation (design 10 item 7). No window yet (the presenter is phase P4).
// Fallback: when no Vulkan device meets the baseline (no loader, no driver, a device below design 9.1), every function takes the null backend's form instead (dm/gfx/nvn/nvn_null.c:
// one [RED] line per function on its first call, a GPU that has finished everything, events and syncs written at once, the pool table kept), so a host without Vulkan still runs
// its CPU side. The two forms live in this one library: a host binary links exactly one backend (dm/host/CMakeLists.txt: nxhost links hostvk; nvn_null.c stays the backend of the
// CMake targets that do not link hostvk, t_frontend).
// Declared here, with a red line and the phase that brings them: presentation, the display link and pacing (P4), the external producers of the guest Vulkan and GL layers (P9),
// be_report's statistics lines (the per-frame statistics answer zero or what the backend counts).
#include "plat/plat.h"
#include "vk_backend.h"
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

// ---------- Bring-up and shutdown ----------
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static char g_why[1024];   // why the device is not up (for the null forms' red line)
static void up_once(void) {
    hostvk_opts o = {-1, 0, 0};
    const char *e = getenv("DM_VK_DEVICE");
    if (e && *e) { char *end; long v = strtol(e, &end, 10); if (*end || v < 0) hostvk_log("[RED] [hostvk] DM_VK_DEVICE=%s is not a device index: the first device that meets the baseline is used", e); else o.device_index = (int)v; }
    e = getenv("DM_VK_VALIDATION");
    o.validation = e && *e && strcmp(e, "0") != 0;
    int rc = hostvk_init(&o, &hvk.dev, g_why, sizeof g_why);
    if (rc) { hostvk_log("[RED] [hostvk] no Vulkan device for the NVN backend (%s): the null backend's forms answer, nothing is drawn", g_why); return; }
    hvk.d = hvk.dev.device; hvk.lim = hvk.dev.props.limits;
    if (!hvk.dev.caps.buffer_device_address) hostvk_log("[RED] [hostvk] the device has no buffer device address: counter reports and conditional rendering are not written by the GPU");
    if (!hvk_commands_init()) { hostvk_log("[RED] [hostvk] the command layer could not be made: the null backend's forms answer"); hostvk_destroy(&hvk.dev); hvk.d = VK_NULL_HANDLE; return; }
    hvk.on = 1;
    hostvk_log("[hostvk] NVN backend on Vulkan: library %s%s", hvk_shader_dir(), o.validation ? ", validation on (DM_VK_VALIDATION)" : "");
}
int hvk_up(void) { pthread_once(&g_once, up_once); return hvk.on; }
// For tests and tools (dm/gfx/tests/vk): the device the backend runs on (its validation counters), NULL when it is not up; and a full shutdown, after which nothing may be called
const hostvk_dev *hvk_device(void);
const hostvk_dev *hvk_device(void) { return hvk_up() ? &hvk.dev : NULL; }
void hvk_shutdown(void);
void hvk_shutdown(void) {
    if (!hvk.on) return;
    hvk_commands_shutdown();
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
// Guest memory committed / about to be decommitted (design 1.5, 1.6; hle_svc.c is to call them with the CPU port): imports are per pool in phase P3, so a commit needs nothing;
// a decommit under a live pool releases its import first (vk_memory.c)
void be_guest_map(void *mem, uint64_t size) { (void)mem; (void)size; if (!hvk.on) NULLRED(); }
void be_guest_unmap(void *mem, uint64_t size) { if (hvk.on) hvk_guest_unmap(mem, size); else NULLRED(); }
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
void be_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }   // the Metal look-ahead's handles still to come: not used
void be_exec(const uint8_t *handle) { if (!hvk.on) NULLRED(); nvn_replay_exec(handle); }   // the frontend's decoder hands every record to the functions below
uint64_t be_sub_commit(void) { if (hvk.on) return hvk_sub_commit(); NULLRED(); return atomic_fetch_add(&g_null_fence, 1) + 1; }
uint64_t be_last_fence(void) { if (hvk.on) return hvk_last(); NULLRED(); return atomic_load(&g_null_fence); }
int be_wait(uint64_t fence, uint64_t timeout_ns) { if (hvk.on) return hvk_wait(fence, timeout_ns); NULLRED(); (void)fence; (void)timeout_ns; return 1; }
// An acquire sync (fenced 2, by WindowAcquireTexture): the window's texture is free once the commits before the acquire completed; headless, that is the fence itself
int be_wait_acq(uint64_t fence, uint64_t timeout_ns) { if (hvk.on) return hvk_wait(fence, timeout_ns); NULLRED(); (void)fence; (void)timeout_ns; return 1; }
void be_finish(void) { if (hvk.on) hvk_finish(); else NULLRED(); }
void be_drain(void) { if (hvk.on) hvk_finish(); else NULLRED(); }

// ---------- Presentation and pacing: phase P4 (the presenter, the vsync source of design 7) ----------
uint64_t be_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)t; (void)period_s; (void)crop;
    if (!hvk.on) { NULLRED(); return 0; }
    DECLARED("present: no presenter yet (phase P4, design 7): the frame is not shown");
    return 0;   // no ticket to wait for
}
void be_present_flush(void) { if (!hvk.on) NULLRED(); }
int be_has_window(void) { return 0; }   // headless (phase P3)
int be_display_hz(void) { return 0; }
int be_vsync_wait_ticks(int ticks, uint64_t timeout_ns) { (void)ticks; (void)timeout_ns; DECLARED("vsync wait: no vsync source yet (phase P4, design 7.3): the caller paces on its own clock"); return 0; }
int be_vsync_rate(void) { return 0; }
int be_vsync_stalled(void) { return 0; }
int be_vsync_tick(uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms) { (void)n; (void)target; (void)lead_ms; (void)ticks; (void)gap_ms; return 0; }
double be_presented_time(uint64_t n) { (void)n; return 0; }   // 0: not shown yet
int be_snap_on(void) { return 0; }
uint64_t be_snap_acquire_wait(void) { return 0; }
void be_present_wait(uint64_t ticket) { (void)ticket; }
int be_fps_target(void) {
    static int env = -2;
    if (env == -2) { const char *e = getenv("DM_FPS"); env = e ? atoi(e) : -1; if (env != 30 && env != 60) env = -1; }
    extern int nvn_game_fps(void) __attribute__((weak));
    return env > 0 ? env : nvn_game_fps && nvn_game_fps() == 30 ? 30 : 60;   // the Metal backend's rule (mtl_fps_target)
}
int be_vsync_active(void) { return 0; }
int be_vsync_wait(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; DECLARED("vsync wait: no vsync source yet (phase P4, design 7.3)"); return 0; }
// External producers (the guest Vulkan and GL layers on this host, phase P9): the shared device record is the backend's device layer
void *be_native_device(void) { return hvk_up() ? (void *)&hvk.dev : NULL; }
uint64_t be_ext_tex(void *tex) { (void)tex; DECLARED("external textures of the guest Vulkan and GL layers: phase P9"); return 0; }
uint64_t be_present_ext(uint64_t serial, double period, void *ev, uint64_t ev_val, const int crop[4]) {
    (void)serial; (void)period; (void)ev; (void)ev_val; (void)crop; DECLARED("present of an external producer's frame: phase P4 / P9"); return 0; }

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
void be_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = 0; *pso_ns = atomic_load(&hvk.st_pipeline_ns); }
void be_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none) { *draws = *made = *none = 0; }
void be_res_stats(char *out, size_t n) {
    if (n) snprintf(out, n, "vulkan: passes %llu draws %llu dispatches %llu pipelines %llu shaders %llu barriers %llu pass splits %llu GPU vertex copies %llu GPU quad indices %llu",
                    (unsigned long long)hvk.st_passes, (unsigned long long)hvk.st_draws, (unsigned long long)hvk.st_dispatches, (unsigned long long)hvk.st_pipelines,
                    (unsigned long long)hvk.st_shaders, (unsigned long long)hvk.st_barriers, (unsigned long long)hvk.st_pass_splits, (unsigned long long)hvk.st_vcopies_gpu,
                    (unsigned long long)hvk.st_index_gpu);
}
void be_barrier_stats(uint64_t *skip, uint64_t *full) { *skip = 0; *full = atomic_load(&hvk.st_barriers); }
uint32_t be_present_qos(void) { return 0; }   // PLAT_QOS_UNSPECIFIED: no present thread yet
uint64_t be_cut_count(void) { return 0; }
uint64_t be_copy_cut_count(void) { return 0; }
uint64_t be_blit_splits(void) { return atomic_load(&hvk.st_pass_splits); }
void be_ubd_stats(uint64_t *deferred, uint64_t *conflicts) { *deferred = *conflicts = 0; }
void be_ubp_stats(uint64_t *made, uint64_t *reused, uint64_t *refused) { *made = *reused = *refused = 0; }
void be_ubp_why(uint64_t w[5]) { memset(w, 0, 5 * sizeof *w); }
void be_lc_stats(uint64_t *taken, uint64_t *landed) { *taken = *landed = 0; }
void be_sm_stats(uint64_t *np, uint64_t *nc, uint64_t *nw) { *np = *nc = *nw = 0; }
void be_cshoist_stats(uint64_t *moved, uint64_t *depth, uint64_t *stops) { *moved = *depth = 0; memset(stops, 0, 8 * sizeof *stops); }   // stops: 8 counters, as hle_nvn.c reads them
void be_pso_line(char *o, size_t n) { if (n) snprintf(o, n, "vulkan: %llu pipelines made in %.1f ms (monolithic, phase P3)", (unsigned long long)hvk.st_pipelines, hvk.st_pipeline_ns / 1e6); }
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
void be_tf_bind(const uint8_t *rec, const op_hdr *h, const void *a) { (void)rec; (void)h; (void)a; }        // the buffers are S.tf's when a capture begins in a pass
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
