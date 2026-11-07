// Metal execution layer of the NVN null device (C interface): memory pool = zero-copy MTLBuffer, submit = MTLCommandBuffer, sync = MTLSharedEvent.
// Ordering convention (to avoid repeating the GO_MAP_SHARE corrupted-picture mistake of the x86 route): every visible effect of "GPU completion" (sync object ready, event values, counters)
// happens only after the Metal command buffer has actually completed; CPU-side waits look only at the completed value of the MTLSharedEvent.
#pragma once
#include <stdint.h>
#include <stddef.h>

void mtl_init(void);
void mtl_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip, uint64_t *copies, uint64_t *copy_bytes);
void mtl_set_depth_zero(int on);          // NVN depth mode: 1 = near is 0 (D3D / Metal), 0 = near is -w (GL, NVN's default); picks the vertex shaders' entry point (see shader_load in nvn_mtl.m)
void mtl_set_origin_lower_left(int on);   // NVN window origin: 1 = lower left (GL, NVN's default), 0 = upper left (D3D / Metal); see nvn_mtl.m
void mtl_set_queue_origin_lower_left(int on); // Raster flip captured at NVN queue creation; selected under the submit lock.
void bf_want_note(const char *key);

// Memory pool: [mem, mem+size) is expanded outward to page boundaries to build a zero-copy buffer; look up the buffer and offset by GPU address (= CPU address)
void mtl_pool_add(const void *mem, uint64_t size);
void mtl_pool_addressed(const void *mem, uint64_t size);   // the game holds a GPU address in this range, so raw global memory may reach it
void mtl_pool_del(const void *mem);
// memory pool Finalize: evict the textures built on this pool (pool object address) from the storage-sharing table
void mtl_alias_drop_pool(const void *pool);
// physical memory pool Finalize: the shared placement sparse tiles of its memory go back once no texture maps them (sp_release)
void mtl_sp_phys_gone(const void *mem, uint64_t size);
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off);   // 1 on success; buf is a (__bridge) id<MTLBuffer>
int mtl_addr_known(uint64_t addr, uint64_t len);   // 1 when the range lies in one memory pool (as mtl_addr finds it); does not make the pool resident

typedef struct {
    uint32_t pso, cps, shaders, dss;
    uint32_t view_nonnull, view_slots, live_tex, sview_nonnull;
    uint32_t pools;
    uint64_t pool_adds;
    uint32_t c1_bufs; uint64_t c1_alloc, c1_data;   // Shader c1 constant segment: buffer count (4 MiB banks, dedicated 64 KiB slices inside them), allocated bytes, actual bytes
} mtl_res_count;
void mtl_res_counts(uint32_t tick, const char *why);   // Print one line of resource counts (the host calls it per tick / event, see dm_res_report_register in dm.h)
void mtl_res_snapshot(mtl_res_count *out);

// One submit: begin -> several operations -> commit (returns the fence value at which this submit completes)
void mtl_prepare(void);                                // the first submission's prepare step (prewarm, pipeline prebuild): no GPU work, called outside the graphics gate
void mtl_sub_begin(void);
void mtl_queue_seal(void);                         // own persistent target snapshots before record retirement
void mtl_sub_resume(void);                         // continuation after a guest event boundary
// Serialized by the submit lock. Event chunks append to one allocator lease;
// pressure may revoke its completed prefix so blocked submissions cannot pin
// the bounded allocator pool forever. Unencoded continuations reacquire it.
typedef struct mtl_submit_alloc mtl_submit_alloc;
mtl_submit_alloc *mtl_submit_alloc_new(void);
void mtl_submit_alloc_close(mtl_submit_alloc *alloc); // last chunk committed, or owner released
void mtl_submit_alloc_release(mtl_submit_alloc *alloc);
void mtl_sub_begin_alloc(mtl_submit_alloc *alloc, int resume);
// Submission-mode timestamps share one clock across host event chunks. The
// logical submission owns the token; set_clock retains it until completion.
// Segment-mode GPU timestamps and direct submits without a token are unchanged.
typedef struct mtl_submit_clock mtl_submit_clock;
mtl_submit_clock *mtl_submit_clock_new(void);
void mtl_submit_clock_release(mtl_submit_clock *clock);
void mtl_sub_set_clock(mtl_submit_clock *clock);    // after begin/resume, before encoding reports
uint64_t mtl_completed_fence(void);                // post-operations are visible through this fence
int mtl_xq_on(void);                               // DM_NVN_XQ_EVENTS (pragmata-r6-q1245)
void mtl_queue_activate(uint64_t serial);              // submit lock held: restore this queue's persistent GPU state
void mtl_queue_forget(uint64_t serial);                // finalize: discard its state, including guest object pointers
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n);               // Buffer copy in GPU ordering
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v);   // Write the event value after completion (0 write / 1 add)
void mtl_sub_counter(uint64_t addr, uint32_t type);                       // Write the counter record after completion
uint64_t mtl_sub_commit(void);

uint64_t mtl_last_fence(void);                       // Fence value of the most recent submit (0 = never submitted)
int mtl_wait(uint64_t fence, uint64_t timeout_ns);   // Returns 1 when the GPU has completed up to fence, 0 on timeout
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns);   // the same for a sync WindowAcquireTexture handed out (its own line in the wait ledger, DM_NVN_WAITS)
void mtl_finish(void);                               // Wait for all submitted commands to complete
uint64_t mtl_done_fence(void);                       // the newest fence the GPU has completed (and its CPU-visible effects published)
int64_t mtl_up_seq_now(void); int mtl_up_busy_through(int64_t seq);   // upload batches: the newest begun; one begun at or before seq still open or running
void mtl_pool_update_begin(void); void mtl_pool_update_end(void);   // around a sparse run update (vrun_set): lookups and the pool table see it whole
int mtl_pool_extend(const void *mem, uint64_t size);  // a large pool grown at its end keeps its entry and buffers (0: not a large pool, delete and add it)
int mtl_sparse_safe(void);                           // DM_NVN_SPARSE_SAFE (default on): zero-copy ranges checked before IOGPU wires them; sparse pools keep pages under live buffers (hle_nvn.c vpool_map)
// expl2 (2026-11-07, nvn_expl2.inc; all off by default): DM_NVN_VA_AUDIT, DM_NVN_REMAP_REFRESH, DM_NVN_ALIAS_FENCE
int mtl_remap_wanted(void);                          // DM_NVN_REMAP_REFRESH or DM_NVN_VA_AUDIT on: vpool_map counts the mapped pages a request replaces
void mtl_remap_note(uint64_t lo, uint64_t n, int kind, uint64_t pages);   // a sparse request replaced `pages` mapped 4 KB pages in [lo, lo + n) (kind 'm' remap, 'u' unmap); any thread
typedef struct { uint64_t first, expect, got, pgen; int kind, bad, pieces, desc, holes; } nvn_va_audit_t;   // kind: 0 fine, 'o' order (other host memory), 's' stale (remapped after its buffer), 'h' hole read through another range
int nvn_sparse_va_audit(uint64_t a, uint64_t n, uint64_t h, uint64_t bgen, nvn_va_audit_t *r);   // hle_nvn.c: -1 outside sparse pools, else the bad pages (r: the first)
void mtl_drain(void);                                // Wait for all commands on the queue (including presents) to complete: submit an empty command buffer and wait for it
void mtl_xv_window_tex(uint64_t serial);   // Register a window (swapchain) texture: the cross-submit validity ledger never collects it
// External producers (the Vulkan layer, dm/gfx/vk): the shared Metal device; a serial for one of their textures; present it once their event reaches ev_val (ticket for mtl_present_wait)
void *mtl_device(void);
void *mtl_device_classic(void);   // the same for a layer that draws only on classic queues: a device without Metal 4 (a virtual Mac) starts too, and its presents are dropped
uint64_t mtl_ext_tex(void *tex);
uint64_t mtl_present_ext(uint64_t serial, double period, void *ev, uint64_t ev_val, const int crop[4]);   // crop: x, y, w, h of the texture to show (NULL = all of it)
void mtl_present_layer(uint32_t w, uint32_t h);   // a sized vi layer (hle_vi.c): the picture's shape instead of the display's 16:9
void mtl_present_screen(uint32_t w, uint32_t h);   // a screen taller than 16:9 (loader.c, DM_SCREEN_ASPECT): the picture's shape for a texture of that shape
void mtl_present_wait(uint64_t ticket);   // as in nvn_obj.h: wait until that present has been handed to the GPU
