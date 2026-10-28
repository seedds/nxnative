// NVN graphics backend interface: what the NVN frontend (hle_nvn.c) calls in the backend that replays its command records (docs/design/vulkan-backend.md, sections 1.5 and 1.7, step M1).
// One backend per host binary, chosen at build time. On Apple every be_* below is a static inline one-line forward to the Metal backend's mtl_* function with the same parameters and
// result (nvn_mtl.m; nvn_obj.h and nvn_mtl.h declare them), so the Metal backend's behaviour is unchanged. Other hosts get the same functions as prototypes, which the Vulkan backend
// (dm/gfx/hostvk, step M7) is to define. Names are the mtl_* ones with be_ in front, except: mtl_link_* (the display link) are be_vsync_*, mtl_device is be_native_device, and
// be_pool_add also takes the pool's NVN flags (memory placement for a Vulkan backend; the Metal backend ignores them). Outside the Apple block this header is plain C11.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "nvn_obj.h"

#if defined(__APPLE__)
#include "nvn_mtl.h"
// Metal backend functions that no header declares (hle_nvn.c declared them where it called them); their definitions in nvn_mtl.m have exactly these types
void mtl_res_stats(char *out, size_t n);
void mtl_barrier_stats(uint64_t *skip, uint64_t *full);
uint32_t mtl_present_qos(void);
void mtl_mem_stat(unsigned long long frame, unsigned long long rss_mb);
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns);
void mtl_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none);
uint64_t mtl_upload_bytes(void);
uint64_t mtl_cut_count(void);
uint64_t mtl_copy_cut_count(void);
uint64_t mtl_blit_splits(void);
void mtl_ubd_stats(uint64_t *deferred, uint64_t *conflicts);
void mtl_ubp_stats(uint64_t *made, uint64_t *reused, uint64_t *refused);
void mtl_lc_stats(uint64_t *taken, uint64_t *landed);
void mtl_ubp_why(uint64_t w[5]);
void mtl_sm_stats(uint64_t *np, uint64_t *nc, uint64_t *nw);
void mtl_cshoist_stats(uint64_t *moved, uint64_t *depth, uint64_t *stops);
void mtl_pso_line(char *o, size_t n);
void mtl_alloc_stats(uint64_t *waits, uint64_t *grown, int *n);

// device
static inline void be_init(void) { mtl_init(); }

// memory pools
static inline void be_pool_add(const void *mem, uint64_t size, uint32_t nvn_flags) { (void)nvn_flags; mtl_pool_add(mem, size); }
static inline void be_pool_del(const void *mem) { mtl_pool_del(mem); }
static inline void be_alias_drop_pool(const void *pool) { mtl_alias_drop_pool(pool); }
static inline int be_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) { return mtl_addr(addr, len, buf, off); }

// object hooks
static inline void be_tex_init(const tex_t *t) { mtl_tex_init(t); }
static inline void be_tex_release(tex_t *t) { mtl_tex_release(t); }
static inline void be_tex_cpu_written(const tex_t *t) { mtl_tex_cpu_written(t); }
static inline int be_tex_gpu_owned(uint64_t serial) { return mtl_tex_gpu_owned(serial); }
static inline void be_reg_tex(int tid, const tex_t *t, const view_t *v) { mtl_reg_tex(tid, t, v); }
static inline void be_reg_image(int tid, const tex_t *t, const view_t *v) { mtl_reg_image(tid, t, v); }
static inline void be_reg_samp(int sid, const samp_par *sp) { mtl_reg_samp(sid, sp); }
static inline void be_set_tex_pool_n(int n) { mtl_set_tex_pool_n(n); }
static inline void be_prog_release(prog_t *p) { mtl_prog_release(p); }
static inline void be_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) { mtl_read_texels(t, level, layer0, r, dst, row, img); }
static inline void be_xv_window_tex(uint64_t serial) { mtl_xv_window_tex(serial); }
static inline void be_set_depth_zero(int on) { mtl_set_depth_zero(on); }
static inline void be_set_origin_lower_left(int on) { mtl_set_origin_lower_left(on); }
static inline void be_set_queue_origin_lower_left(int on) { mtl_set_queue_origin_lower_left(on); }

// submission and sync (hle_nvn.c, under its submission lock)
static inline void be_queue_activate(uint64_t serial) { mtl_queue_activate(serial); }
static inline void be_queue_forget(uint64_t serial) { mtl_queue_forget(serial); }
static inline void be_sub_begin(void) { mtl_sub_begin(); }
static inline void be_sub_index(const uint64_t *h, int n) { mtl_sub_index(h, n); }
static inline void be_exec_rest(const uint64_t *h, int n) { mtl_exec_rest(h, n); }
static inline void be_exec(const uint8_t *handle) { mtl_exec(handle); }
static inline uint64_t be_sub_commit(void) { return mtl_sub_commit(); }
static inline uint64_t be_last_fence(void) { return mtl_last_fence(); }
static inline int be_wait(uint64_t fence, uint64_t timeout_ns) { return mtl_wait(fence, timeout_ns); }
static inline int be_wait_acq(uint64_t fence, uint64_t timeout_ns) { return mtl_wait_acq(fence, timeout_ns); }
static inline void be_finish(void) { mtl_finish(); }
static inline void be_drain(void) { mtl_drain(); }

// presentation and pacing
static inline uint64_t be_present(const tex_t *t, double period_s, const int crop[4]) { return mtl_present(t, period_s, crop); }
static inline void be_present_wait(uint64_t ticket) { mtl_present_wait(ticket); }
static inline void be_present_flush(void) { mtl_present_flush(); }
static inline int be_has_window(void) { return mtl_has_window(); }
static inline int be_display_hz(void) { return mtl_display_hz(); }
static inline int be_fps_target(void) { return mtl_fps_target(); }
static inline int be_vsync_active(void) { return mtl_link_active(); }
static inline int be_vsync_wait(int vsyncs, uint64_t timeout_ns) { return mtl_link_wait_tick(vsyncs, timeout_ns); }
static inline int be_vsync_wait_ticks(int ticks, uint64_t timeout_ns) { return mtl_link_wait_ticks(ticks, timeout_ns); }
static inline int be_vsync_rate(void) { return mtl_link_rate(); }
static inline int be_vsync_stalled(void) { return mtl_link_stalled(); }
static inline int be_vsync_tick(uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms) { return mtl_link_tick(n, target, lead_ms, ticks, gap_ms); }
static inline double be_presented_time(uint64_t n) { return mtl_presented_time(n); }
static inline int be_snap_on(void) { return mtl_snap_on(); }
static inline uint64_t be_snap_acquire_wait(void) { return mtl_snap_acquire_wait(); }

// external producers (the guest Vulkan and GL layers on the same host; they still call the mtl_* names, see the end of this file)
static inline void *be_native_device(void) { return mtl_device(); }
static inline uint64_t be_ext_tex(void *tex) { return mtl_ext_tex(tex); }
static inline uint64_t be_present_ext(uint64_t serial, double period, void *ev, uint64_t ev_val, const int crop[4]) { return mtl_present_ext(serial, period, ev, ev_val, crop); }

// frames and measurements
static inline void be_set_frame(uint64_t f) { mtl_set_frame(f); }
static inline double be_gpu_ms(uint64_t f) { return mtl_gpu_ms(f); }
static inline int be_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { return mtl_gpu_span(f, t0, t1); }
static inline void be_ab_gpu(uint64_t f, double gpu_ms, double frame_ms) { mtl_ab_gpu(f, gpu_ms, frame_ms); }
static inline void be_ub_present(double iv_ms) { mtl_ub_present(iv_ms); }

// statistics for hle_nvn.c's log lines and per-frame rows (be_report takes over the log lines later, see the end of this file)
static inline void be_mem_stat(unsigned long long frame, unsigned long long rss_mb) { mtl_mem_stat(frame, rss_mb); }
static inline void be_stats(uint64_t *passes, uint64_t *draws) { mtl_stats(passes, draws); }
static inline void be_args_stats(uint64_t *enc, uint64_t *reuse) { mtl_args_stats(enc, reuse); }
static inline void be_use_stats(uint64_t *call, uint64_t *skip) { mtl_use_stats(call, skip); }
static inline void be_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip, uint64_t *copies, uint64_t *copy_bytes) { mtl_vbuf_stats(resolve, reuse, set, skip, copies, copy_bytes); }
static inline uint64_t be_ds_omit(void) { return mtl_ds_omit(); }
static inline uint64_t be_drawable_ns_take(void) { return mtl_drawable_ns_take(); }
static inline void be_bw(uint64_t *load, uint64_t *store, uint64_t *full_rw_passes) { mtl_bw(load, store, full_rw_passes); }
static inline uint64_t be_udc_saved_bytes(void) { return mtl_udc_saved_bytes(); }
static inline void be_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { mtl_pkg_stats(uploads, reuploads); }
static inline unsigned be_cpu_tex_count(void) { return mtl_cpu_tex_count(); }
static inline uint64_t be_pkg_edge_skips(void) { return mtl_pkg_edge_skips(); }
static inline uint64_t be_pkg_miss(void) { return mtl_pkg_miss(); }
static inline uint64_t be_upload_ns_take(void) { return mtl_upload_ns_take(); }
static inline uint64_t be_upload_gpu_ns_take(void) { return mtl_upload_gpu_ns_take(); }
static inline uint64_t be_upload_bytes(void) { return mtl_upload_bytes(); }
static inline void be_create_stats(uint64_t *lib, uint64_t *pso) { mtl_create_stats(lib, pso); }
static inline void be_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { mtl_create_ns(lib_ns, pso_ns); }
static inline void be_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none) { mtl_hybrid_stats(draws, made, none); }
static inline void be_res_stats(char *out, size_t n) { mtl_res_stats(out, n); }
static inline void be_barrier_stats(uint64_t *skip, uint64_t *full) { mtl_barrier_stats(skip, full); }
static inline uint32_t be_present_qos(void) { return mtl_present_qos(); }
static inline uint64_t be_cut_count(void) { return mtl_cut_count(); }
static inline uint64_t be_copy_cut_count(void) { return mtl_copy_cut_count(); }
static inline uint64_t be_blit_splits(void) { return mtl_blit_splits(); }
static inline void be_ubd_stats(uint64_t *deferred, uint64_t *conflicts) { mtl_ubd_stats(deferred, conflicts); }
static inline void be_ubp_stats(uint64_t *made, uint64_t *reused, uint64_t *refused) { mtl_ubp_stats(made, reused, refused); }
static inline void be_ubp_why(uint64_t w[5]) { mtl_ubp_why(w); }
static inline void be_lc_stats(uint64_t *taken, uint64_t *landed) { mtl_lc_stats(taken, landed); }
static inline void be_sm_stats(uint64_t *np, uint64_t *nc, uint64_t *nw) { mtl_sm_stats(np, nc, nw); }
static inline void be_cshoist_stats(uint64_t *moved, uint64_t *depth, uint64_t *stops) { mtl_cshoist_stats(moved, depth, stops); }
static inline void be_pso_line(char *o, size_t n) { mtl_pso_line(o, n); }
static inline void be_alloc_stats(uint64_t *waits, uint64_t *grown, int *n) { mtl_alloc_stats(waits, grown, n); }

#else   // other hosts: the backend defines these (the same functions, in the same order, as the Apple block above)

// device
void be_init(void);

// memory pools
void be_pool_add(const void *mem, uint64_t size, uint32_t nvn_flags);
void be_pool_del(const void *mem);
void be_alias_drop_pool(const void *pool);
int be_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off);

// object hooks
void be_tex_init(const tex_t *t);
void be_tex_release(tex_t *t);
void be_tex_cpu_written(const tex_t *t);
int be_tex_gpu_owned(uint64_t serial);
void be_reg_tex(int tid, const tex_t *t, const view_t *v);
void be_reg_image(int tid, const tex_t *t, const view_t *v);
void be_reg_samp(int sid, const samp_par *sp);
void be_set_tex_pool_n(int n);
void be_prog_release(prog_t *p);
void be_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img);
void be_xv_window_tex(uint64_t serial);
void be_set_depth_zero(int on);
void be_set_origin_lower_left(int on);
void be_set_queue_origin_lower_left(int on);

// submission and sync (hle_nvn.c, under its submission lock)
void be_queue_activate(uint64_t serial);
void be_queue_forget(uint64_t serial);
void be_sub_begin(void);
void be_sub_index(const uint64_t *h, int n);
void be_exec_rest(const uint64_t *h, int n);
void be_exec(const uint8_t *handle);
uint64_t be_sub_commit(void);
uint64_t be_last_fence(void);
int be_wait(uint64_t fence, uint64_t timeout_ns);
int be_wait_acq(uint64_t fence, uint64_t timeout_ns);
void be_finish(void);
void be_drain(void);

// presentation and pacing
uint64_t be_present(const tex_t *t, double period_s, const int crop[4]);
void be_present_wait(uint64_t ticket);
void be_present_flush(void);
int be_has_window(void);
int be_display_hz(void);
int be_fps_target(void);
int be_vsync_active(void);
int be_vsync_wait(int vsyncs, uint64_t timeout_ns);
int be_vsync_wait_ticks(int ticks, uint64_t timeout_ns);
int be_vsync_rate(void);
int be_vsync_stalled(void);
int be_vsync_tick(uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms);
double be_presented_time(uint64_t n);
int be_snap_on(void);
uint64_t be_snap_acquire_wait(void);

// external producers
void *be_native_device(void);
uint64_t be_ext_tex(void *tex);
uint64_t be_present_ext(uint64_t serial, double period, void *ev, uint64_t ev_val, const int crop[4]);

// frames and measurements
void be_set_frame(uint64_t f);
double be_gpu_ms(uint64_t f);
int be_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1);
void be_ab_gpu(uint64_t f, double gpu_ms, double frame_ms);
void be_ub_present(double iv_ms);

// statistics
void be_mem_stat(unsigned long long frame, unsigned long long rss_mb);
void be_stats(uint64_t *passes, uint64_t *draws);
void be_args_stats(uint64_t *enc, uint64_t *reuse);
void be_use_stats(uint64_t *call, uint64_t *skip);
void be_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip, uint64_t *copies, uint64_t *copy_bytes);
uint64_t be_ds_omit(void);
uint64_t be_drawable_ns_take(void);
void be_bw(uint64_t *load, uint64_t *store, uint64_t *full_rw_passes);
uint64_t be_udc_saved_bytes(void);
void be_pkg_stats(uint64_t *uploads, uint64_t *reuploads);
unsigned be_cpu_tex_count(void);
uint64_t be_pkg_edge_skips(void);
uint64_t be_pkg_miss(void);
uint64_t be_upload_ns_take(void);
uint64_t be_upload_gpu_ns_take(void);
uint64_t be_upload_bytes(void);
void be_create_stats(uint64_t *lib, uint64_t *pso);
void be_create_ns(uint64_t *lib_ns, uint64_t *pso_ns);
void be_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none);
void be_res_stats(char *out, size_t n);
void be_barrier_stats(uint64_t *skip, uint64_t *full);
uint32_t be_present_qos(void);
uint64_t be_cut_count(void);
uint64_t be_copy_cut_count(void);
uint64_t be_blit_splits(void);
void be_ubd_stats(uint64_t *deferred, uint64_t *conflicts);
void be_ubp_stats(uint64_t *made, uint64_t *reused, uint64_t *refused);
void be_ubp_why(uint64_t w[5]);
void be_lc_stats(uint64_t *taken, uint64_t *landed);
void be_sm_stats(uint64_t *np, uint64_t *nc, uint64_t *nw);
void be_cshoist_stats(uint64_t *moved, uint64_t *depth, uint64_t *stops);
void be_pso_line(char *o, size_t n);
void be_alloc_stats(uint64_t *waits, uint64_t *grown, int *n);

#endif

// Not part of step M1 (docs/design/vulkan-backend.md, section 1.5); none of these is declared yet:
// - With the Vulkan backend: const be_caps *be_get_caps(void) (device capabilities); void be_guest_map(void *mem, uint64_t size) and void be_guest_unmap(void *mem, uint64_t size)
//   (guest memory committed / about to be decommitted, called from hle_svc.c, so the backend can release an import first).
// - With step M2: be_addr goes; the frontend's pool table answers it (nvn_pool_lookup, nvn_pools.c).
// - With step M3: the per-operation group, called by the frontend's decoder (nvn_replay.c) in record order with what today's case arms of mtl_exec receive: be_rec_pre, be_state_dirty,
//   be_set_rt, be_clear_color, be_clear_ds, be_draw, be_dispatch, be_copy_b2b, be_update_ub, be_fill, be_copy_b2t, be_copy_t2b, be_copy_t2t, be_clear_tex, be_barrier,
//   be_report_counter, be_reset_counter, be_signal_event, be_fence_sync, be_render_cond, be_tf_state; be_exec and be_exec_rest then go.
// - void be_report(uint32_t tick, char *out, size_t n): designed, not yet used. The backend is to format its own statistics lines, replacing the 41 calls in hle_nvn.c's log lines and
//   per-frame rows of these 33 functions: be_mem_stat, be_stats, be_args_stats, be_use_stats, be_vbuf_stats, be_ds_omit, be_drawable_ns_take, be_bw, be_udc_saved_bytes, be_pkg_stats,
//   be_cpu_tex_count, be_pkg_edge_skips, be_pkg_miss, be_upload_ns_take, be_upload_gpu_ns_take, be_upload_bytes, be_create_stats, be_create_ns, be_hybrid_stats, be_res_stats,
//   be_barrier_stats, be_present_qos, be_cut_count, be_copy_cut_count, be_blit_splits, be_ubd_stats, be_ubp_stats, be_ubp_why, be_lc_stats, be_sm_stats, be_cshoist_stats,
//   be_pso_line, be_alloc_stats. Several of them also feed the per-frame TSV rows (DM_NVN_TSV: @upload_ms, @drawable_ms, @ds_omit, ...), which a tick-based be_report does not cover.
// - The guest Vulkan and GL layers (dm/gfx/vk/hle_vk.m, dm/gfx/gl/hle_gl.m) and the host (dm/host/main.c, hle_sdk16.c, dm/ios/Sources/front_ios.m) still call mtl_* names, some with no
//   be_* counterpart yet (mtl_device_classic, mtl_link_wait_vsync, mtl_pso_prebuild_tool, mtl_present_count): step M6 (the build split) moves them.
