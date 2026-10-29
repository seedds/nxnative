// The null graphics backend (docs/design/vulkan-backend.md, step M6): every function of the backend interface (nvn_be.h, nvn_be_ext.h: the wrappers and the per-operation group)
// and the hooks the frontend's state, handle and pool code calls in its backend (nvn_state.h, nvn_resolve.h, nvn_pools.h), for the hosts that have no GPU backend yet: Linux and
// Windows, until the Vulkan backend (dm/gfx/hostvk, step M7) takes its place. It draws nothing. Each function says so on its first call, with one [RED] line naming it, and gives the
// neutral answer: no window and no display link, a GPU that has finished everything it was given (a submission's fence is complete at once, so a wait never blocks), statistics of
// zero. What NVN makes visible to the CPU when the GPU reaches a command is done at once: an event's value and a sync object's fence, so a game that waits on them goes on to report
// its next missing piece instead of hanging. The memory pool table (nvn_pools.c) is kept, under this backend's lock and with no object of its own per pool, so the frontend's address
// lookups (be_addr) answer as with a real backend; the per-queue state (nvn_state.c) is switched as the Metal backend switches it. The decoder runs (be_exec: nvn_replay_exec), so a
// submitted command list reaches the per-operation functions below in record order.
// Built only where there is no Metal (dm/host/CMakeLists.txt); the Apple Makefiles leave it out, and on Apple the file is empty, since nvn_be.h's functions are the Metal backend's
#if !defined(__APPLE__)
#include "nvn_be.h"
#include "nvn_pools.h"
#include "nvn_replay.h"
#include "nvn_resolve.h"
#include "nvn_state.h"
#include "nvn_tex.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

void dm_guest_write_prepare(void *addr, size_t len);   // the host's pre-write hook (dm/host/dm.h): before the host itself writes guest memory a dirty-page tracker may have protected

// ---------- The red line, once per function, and what a test reads back ----------
enum { NULL_SEEN_MAX = 256 };
static const char *_Atomic g_seen[NULL_SEEN_MAX];   // the functions reached, in the order of their first call
static _Atomic int g_nseen;
static _Atomic uint64_t g_calls;
static void reached(_Atomic int *once, const char *fn) {
    atomic_fetch_add(&g_calls, 1);
    if (atomic_exchange(once, 1)) return;
    int i = atomic_fetch_add(&g_nseen, 1);
    if (i < NULL_SEEN_MAX) atomic_store(&g_seen[i], fn);
    fprintf(stderr, "[dm] [RED] null graphics backend: %s called, and no GPU work is done for it (no GPU backend on this platform yet; the Vulkan backend is step M7)\n", fn);
}
#define RED() do { static _Atomic int once_; reached(&once_, __func__); } while (0)
// For tests (dm/gfx/tests/t_frontend.c): whether the function named fn (its C name) has been called, how many different functions have been, and the calls in all
int nvn_null_reached(const char *fn) {
    int n = atomic_load(&g_nseen); if (n > NULL_SEEN_MAX) n = NULL_SEEN_MAX;
    for (int i = 0; i < n; i++) { const char *s = atomic_load(&g_seen[i]); if (s && !strcmp(s, fn)) return 1; }
    return 0;
}
int nvn_null_functions(void) { int n = atomic_load(&g_nseen); return n > NULL_SEEN_MAX ? NULL_SEEN_MAX : n; }
uint64_t nvn_null_calls(void) { return atomic_load(&g_calls); }

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   // this backend's lock: the pool table's (nvn_pools.h), as the Metal backend's g_mu is
static _Atomic uint64_t g_fence;                           // the last submission's fence value: complete as soon as it is given

// ---------- Device ----------
void be_init(void) { RED(); }
const be_caps *be_get_caps(void) { RED(); static const be_caps none; return &none; }   // no GPU: every capability 0

// ---------- Memory pools: the shared table, with the pool's own address as its GPU address and no backend object ----------
void be_pool_add(const void *mem, uint64_t size, uint32_t nvn_flags) {
    RED();
    pthread_mutex_lock(&g_mu); nvn_pool_add(mem, size, nvn_flags, (uintptr_t)mem, NULL, (uint64_t)(uintptr_t)mem); pthread_mutex_unlock(&g_mu);
}
void be_pool_del(const void *mem) { RED(); pthread_mutex_lock(&g_mu); nvn_pool_del(mem); pthread_mutex_unlock(&g_mu); }
void be_alias_drop_pool(const void *pool) { RED(); (void)pool; }
void be_guest_map(void *mem, uint64_t size) { RED(); (void)mem; (void)size; }     // no import to make or release
void be_guest_unmap(void *mem, uint64_t size) { RED(); (void)mem; (void)size; }
int be_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    RED();
    const nvn_pool_ref *c = nvn_pool_cached(addr, len);
    if (c) { *buf = c->backend; *off = addr - c->base; return 1; }
    int ok = 0; nvn_pool_ref r;
    pthread_mutex_lock(&g_mu);
    if (nvn_pool_lookup(addr, len, &r)) { *buf = r.backend; *off = addr - r.base; ok = 1; }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
void nvn_pool_hook_use(nvn_pool *p) { RED(); (void)p; }   // no residency to make
void nvn_pool_hook_del(nvn_pool *p) { RED(); (void)p; }   // no object to release

// ---------- Object hooks ----------
void be_tex_init(const tex_t *t) { RED(); (void)t; }
void be_tex_release(tex_t *t) { RED(); (void)t; }
void be_tex_cpu_written(const tex_t *t) { RED(); (void)t; }
int be_tex_gpu_owned(uint64_t serial) { RED(); (void)serial; return 0; }
void be_reg_tex(int tid, const tex_t *t, const view_t *v) { RED(); (void)tid; (void)t; (void)v; }
void be_reg_image(int tid, const tex_t *t, const view_t *v) { RED(); (void)tid; (void)t; (void)v; }
void be_reg_samp(int sid, const samp_par *sp) { RED(); (void)sid; (void)sp; }
void be_set_tex_pool_n(int n) { RED(); (void)n; }
void be_prog_release(prog_t *p) { RED(); (void)p; }
// No texture holds GPU content: the texels read are zero (only the region's rows are written, each image's at its stride)
void be_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    RED(); (void)level; (void)layer0;
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb || !dst || r->width <= 0 || r->height <= 0) return;
    size_t wb = (size_t)((r->width + bw - 1) / bw) * (size_t)bpb, hb = (size_t)((r->height + bh - 1) / bh), nz = r->depth > 0 ? (size_t)r->depth : 1;
    for (size_t z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memset(dst + z * img + y * row, 0, wb);
}
void be_xv_window_tex(uint64_t serial) { RED(); (void)serial; }
void be_set_depth_zero(int on) { RED(); (void)on; }
void be_set_queue_origin_lower_left(int on) { RED(); (void)on; }

// ---------- Submission and sync (hle_nvn.c holds its submission lock) ----------
void be_queue_activate(uint64_t serial) { RED(); (void)nvn_state_queue_activate(serial); }   // the frontend's per-queue state, as the Metal backend switches it (no flags to set)
void be_queue_forget(uint64_t serial) { RED(); (void)nvn_state_queue_forget(serial); }
void be_sub_begin(void) { RED(); }
void be_sub_index(const uint64_t *h, int n) { RED(); (void)h; (void)n; }
void be_exec_rest(const uint64_t *h, int n) { RED(); (void)h; (void)n; }
void be_exec(const uint8_t *handle) { RED(); nvn_replay_exec(handle); }   // the frontend's decoder hands every record to the functions below
uint64_t be_sub_commit(void) { RED(); return atomic_fetch_add(&g_fence, 1) + 1; }
uint64_t be_last_fence(void) { RED(); return atomic_load(&g_fence); }
int be_wait(uint64_t fence, uint64_t timeout_ns) { RED(); (void)fence; (void)timeout_ns; return 1; }       // complete: nothing runs
int be_wait_acq(uint64_t fence, uint64_t timeout_ns) { RED(); (void)fence; (void)timeout_ns; return 1; }
void be_finish(void) { RED(); }
void be_drain(void) { RED(); }

// ---------- Presentation and pacing: no window, no display link ----------
uint64_t be_present(const tex_t *t, double period_s, const int crop[4]) { RED(); (void)t; (void)period_s; (void)crop; return 0; }   // no ticket to wait for
void be_present_flush(void) { RED(); }
int be_has_window(void) { RED(); return 0; }
int be_display_hz(void) { RED(); return 0; }
int be_vsync_wait_ticks(int ticks, uint64_t timeout_ns) { RED(); (void)ticks; (void)timeout_ns; return 0; }
int be_vsync_rate(void) { RED(); return 0; }
int be_vsync_stalled(void) { RED(); return 0; }
int be_vsync_tick(uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms) { RED(); (void)n; (void)target; (void)lead_ms; (void)ticks; (void)gap_ms; return 0; }
double be_presented_time(uint64_t n) { RED(); (void)n; return 0; }   // 0: not shown yet
int be_snap_on(void) { RED(); return 0; }
uint64_t be_snap_acquire_wait(void) { RED(); return 0; }
// nvn_be_ext.h: also called by the guest Vulkan and GL layers
void be_set_origin_lower_left(int on) { RED(); (void)on; }
void be_present_wait(uint64_t ticket) { RED(); (void)ticket; }
int be_fps_target(void) { RED(); return 60; }   // the default target, as the Metal backend's without a game asking for 30
int be_vsync_active(void) { RED(); return 0; }
int be_vsync_wait(int vsyncs, uint64_t timeout_ns) { RED(); (void)vsyncs; (void)timeout_ns; return 0; }
void *be_native_device(void) { RED(); return NULL; }
uint64_t be_ext_tex(void *tex) { RED(); (void)tex; return 0; }
uint64_t be_present_ext(uint64_t serial, double period, void *ev, uint64_t ev_val, const int crop[4]) { RED(); (void)serial; (void)period; (void)ev; (void)ev_val; (void)crop; return 0; }

// ---------- Frames and measurements ----------
void be_set_frame(uint64_t f) { RED(); (void)f; }
double be_gpu_ms(uint64_t f) { RED(); (void)f; return -1; }   // no GPU time
int be_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { RED(); (void)f; (void)t0; (void)t1; return 0; }
void be_ab_gpu(uint64_t f, double gpu_ms, double frame_ms) { RED(); (void)f; (void)gpu_ms; (void)frame_ms; }
void be_ub_present(double iv_ms) { RED(); (void)iv_ms; }

// ---------- Statistics: zero ----------
void be_mem_stat(unsigned long long frame, unsigned long long rss_mb) { RED(); (void)frame; (void)rss_mb; }
void be_stats(uint64_t *passes, uint64_t *draws) { RED(); *passes = *draws = 0; }
void be_args_stats(uint64_t *enc, uint64_t *reuse) { RED(); *enc = *reuse = 0; }
void be_use_stats(uint64_t *call, uint64_t *skip) { RED(); *call = *skip = 0; }
void be_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip, uint64_t *copies, uint64_t *copy_bytes) { RED(); *resolve = *reuse = *set = *skip = *copies = *copy_bytes = 0; }
uint64_t be_ds_omit(void) { RED(); return 0; }
uint64_t be_drawable_ns_take(void) { RED(); return 0; }
void be_bw(uint64_t *load, uint64_t *store, uint64_t *full_rw_passes) { RED(); *load = *store = *full_rw_passes = 0; }
uint64_t be_udc_saved_bytes(void) { RED(); return 0; }
void be_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { RED(); *uploads = *reuploads = 0; }
unsigned be_cpu_tex_count(void) { RED(); return 0; }
uint64_t be_pkg_edge_skips(void) { RED(); return 0; }
uint64_t be_pkg_miss(void) { RED(); return 0; }
uint64_t be_upload_ns_take(void) { RED(); return 0; }
uint64_t be_upload_gpu_ns_take(void) { RED(); return 0; }
uint64_t be_upload_bytes(void) { RED(); return 0; }
void be_create_stats(uint64_t *lib, uint64_t *pso) { RED(); *lib = *pso = 0; }
void be_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { RED(); *lib_ns = *pso_ns = 0; }
void be_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none) { RED(); *draws = *made = *none = 0; }
void be_res_stats(char *out, size_t n) { RED(); if (n) snprintf(out, n, "null backend: no GPU resources"); }
void be_barrier_stats(uint64_t *skip, uint64_t *full) { RED(); *skip = *full = 0; }
uint32_t be_present_qos(void) { RED(); return 0; }   // PLAT_QOS_UNSPECIFIED: no present thread
uint64_t be_cut_count(void) { RED(); return 0; }
uint64_t be_copy_cut_count(void) { RED(); return 0; }
uint64_t be_blit_splits(void) { RED(); return 0; }
void be_ubd_stats(uint64_t *deferred, uint64_t *conflicts) { RED(); *deferred = *conflicts = 0; }
void be_ubp_stats(uint64_t *made, uint64_t *reused, uint64_t *refused) { RED(); *made = *reused = *refused = 0; }
void be_ubp_why(uint64_t w[5]) { RED(); memset(w, 0, 5 * sizeof *w); }
void be_lc_stats(uint64_t *taken, uint64_t *landed) { RED(); *taken = *landed = 0; }
void be_sm_stats(uint64_t *np, uint64_t *nc, uint64_t *nw) { RED(); *np = *nc = *nw = 0; }
void be_cshoist_stats(uint64_t *moved, uint64_t *depth, uint64_t *stops) { RED(); *moved = *depth = 0; memset(stops, 0, 8 * sizeof *stops); }   // stops: 8 counters, as hle_nvn.c reads them
void be_pso_line(char *o, size_t n) { RED(); if (n) snprintf(o, n, "null backend: no pipelines"); }
void be_alloc_stats(uint64_t *waits, uint64_t *grown, int *n) { RED(); *waits = *grown = 0; *n = 0; }

// ---------- Per operation (nvn_replay_exec, in record order): nothing is drawn, copied or cleared ----------
int be_list_begin(void) { RED(); return 0; }
int be_rec_pre(const uint8_t *rec, const op_hdr *h, int list) { RED(); (void)rec; (void)h; (void)list; return 0; }   // nothing consumed: every record reaches the decoder
void be_state_dirty(uint32_t mask) { RED(); (void)mask; }
void be_end(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
// The event's value as the GPU would leave it when it reaches the command (mode write or add, as hle_nvn.c's ev_signal), at once: the replay is the GPU here
void be_signal_event(const uint8_t *rec, const op_hdr *h, const void *a) {
    RED(); (void)rec; (void)h;
    volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, (const uint8_t *)a + 8, 4); memcpy(&v, (const uint8_t *)a + 12, 4);
    if (!val) return;
    dm_guest_write_prepare((void *)(uintptr_t)val, 4);
    if (m == NVN_EVENT_SIGNAL_MODE_ADD) *val += v; else *val = v;
}
void be_report_counter(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }   // no counter record is written
void be_render_cond(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
// The sync object is fenced with the submission that reaches it, as the Metal backend fences it: that fence is complete at once (be_wait)
void be_fence_sync(const uint8_t *rec, const op_hdr *h, const void *a) {
    RED(); (void)rec; (void)h;
    sync_t *s; memcpy(&s, a, 8);
    if (s) { s->fence = atomic_load(&g_fence) + 1; s->fenced = 1; }
}
void be_reset_counter(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_fill(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_copy_b2b(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_update_ub(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_set_rt(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_clear_color(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_clear_ds(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_draw(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_draw_idx(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_bind_program(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_tf_bind(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_tf_state_note(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_tf_state(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_rast_discard(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_copy_b2t(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_copy_t2b(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_copy_t2t(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_clear_tex(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_dispatch(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }
void be_barrier(const uint8_t *rec, const op_hdr *h, const void *a) { RED(); (void)rec; (void)h; (void)a; }

// ---------- The frontend's hooks into its backend ----------
uint64_t nvn_state_hook_compose(uint64_t old_serial, const prog_t *const bst[5]) { RED(); (void)old_serial; (void)bst; return nvn_new_serial(); }   // a combined program's serial, with no backend object
void nvn_state_hook_bind(uint32_t op, const p_bind_handles *b) { RED(); (void)op; (void)b; }
void nvn_state_hook_bind_bad(const void *rec, int n) { RED(); (void)rec; (void)n; }
void nvn_state_hook_img_range(const p_bind_handles *b) { RED(); (void)b; }
int nvn_resolve_hook_ubo_copy(uint64_t a, size_t n, void *out) { RED(); (void)a; (void)n; (void)out; return 0; }   // no copies of inline updates
void nvn_resolve_hook_ubo_stale(int stage, int reg, int off, int kind) { RED(); (void)stage; (void)reg; (void)off; (void)kind; }
void nvn_resolve_hook_mem_read(uint64_t a, size_t n, void *out) { RED(); (void)a; (void)n; (void)out; }   // out is left as it is, as for an address in no pool
#endif
