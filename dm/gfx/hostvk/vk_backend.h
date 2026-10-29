// The Vulkan backend of the NVN frontend (docs/design/vulkan-backend.md, phase P3): what its files share. The entry points are the functions of dm/gfx/nvn/nvn_be.h, all defined in
// vk_backend.c; each concern has a file of its own (memory and pools vk_memory.c, textures and samplers vk_texture.c, shader libraries vk_shader.c, pipelines and descriptors
// vk_pipeline.c, the command stream vk_commands.c) and is reached through the hvk_* functions below. Plain C11 over volk, no window: headless until the presenter (phase P4).
//
// Threads. The replay (every per-operation function of nvn_be.h and the submission calls) runs on the submit thread under hle_nvn.c's submission lock. Game threads also call in:
// pools (be_pool_add / be_pool_del), textures (be_tex_init / release / cpu_written, be_reg_*), CPU reads (be_read_texels), waits (be_wait, be_finish). Locks: hvk.mu guards the
// pool table (nvn_pools.h's rule: the backend's lock) and what the backend keeps per pool; hvk.tex_mu the texture table, the texture and sampler registries and the samplers;
// hvk.queue_mu the VkQueue (the submit thread's submissions and the CPU reads' one-off copies). The completion thread (vk_commands.c) runs each finished submission's CPU work
// (events, counter reports the GPU does not write, readbacks) and publishes the fence.
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include "vk_device.h"
#include "vk_import.h"
#include "../nvn/nvn_obj.h"
#include "../nvn/nvn_refl.h"
#include "../nvn/nvn_hmap.h"
#include "../tex/nvn_fmt_vk.h"

// One [RED] line per call site, the first time (the null backend's rule: an unimplemented path says so once)
#define HVK_RED_ONCE(...) do { static _Atomic int once_; if (!atomic_exchange(&once_, 1)) hostvk_log("[RED] [hostvk] " __VA_ARGS__); } while (0)
#define HVK_NOTE_ONCE(...) do { static _Atomic int once_; if (!atomic_exchange(&once_, 1)) hostvk_log("[hostvk] " __VA_ARGS__); } while (0)

// ---------- Memory (vk_memory.c; design 4.2, 4.3) ----------
// A memory pool: the guest storage imported (vk_import.c), rounded out to the import alignment, with one VkBuffer over it. nvn_pool.backend points at it
typedef struct hvk_pool {
    hostvk_import_t im;
    uintptr_t lo, hi;          // the pool as the game gave it, [mem, mem + size)
    uint32_t nvn_flags;
} hvk_pool;
// Guest address → (buffer, offset); also its device address (0 without buffer device address). 0 when no pool holds [addr, addr + len)
int hvk_buf(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off, VkDeviceAddress *dev);
// A ring allocation of the open submission (staging, driver banks, query results, converted indices): host-visible and coherent, alive until the submission completes
typedef struct { VkBuffer buf; VkDeviceSize off; uint8_t *cpu; VkDeviceAddress dev; } hvk_alloc;
int hvk_ring_alloc(VkDeviceSize n, VkDeviceSize align, hvk_alloc *out);   // 0 = out of memory (logged)
// A buffer of the backend's own with memory: host_visible maps it (and *map is set)
VkResult hvk_buffer_make(VkDeviceSize n, VkBufferUsageFlags usage, int host_visible, VkBuffer *buf, VkDeviceMemory *mem, void **map);
void hvk_buffer_free(VkBuffer buf, VkDeviceMemory mem);
uint32_t hvk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback);   // UINT32_MAX for none
VkBufferUsageFlags hvk_ring_usage(void);
// The objects the GPU may still use: released when every submission up to the fence that may use them has completed (hvk_deferred_run)
typedef enum { HVK_DEL_IMPORT, HVK_DEL_BUFFER, HVK_DEL_IMAGE, HVK_DEL_VIEW, HVK_DEL_BUFVIEW, HVK_DEL_FREE, HVK_DEL_PIPELINE } hvk_del_kind;
void hvk_defer(hvk_del_kind kind, void *obj, uint64_t a, uint64_t b);   // a, b: the second and third handles (an image's memory) or a pointer to free
void hvk_deferred_run(uint64_t done);   // release what the fences up to done let go
void hvk_mem_shutdown(void);
// The ring: per submission slot (vk_commands.c owns the slots)
typedef struct hvk_chunk hvk_chunk;
void hvk_ring_slot_release(hvk_chunk **chunks);   // the slot's chunks go back to the free list (its submission completed)
void hvk_ring_bind(hvk_chunk **chunks);           // allocations go to this list from now on (be_sub_begin)

// ---------- Textures and samplers (vk_texture.c; design 5.1, 5.3, 5.4, 5.5) ----------
enum { HVK_VIEW_SAMPLED, HVK_VIEW_ATTACH, HVK_VIEW_STORAGE };
typedef struct hvk_tex hvk_tex;
typedef struct {
    VkImageView view;          // VK_NULL_HANDLE when the view could not be made (logged)
    VkFormat format;
    VkImageAspectFlags aspect;
    uint32_t level, layer0, layers;   // the subresources of the view (an attachment view: one level)
    uint32_t w, h;             // that level's size
    uint8_t cls;               // NVN_VK_FLOAT / UINT / SINT / DS of the view's format
    VkSampleCountFlagBits samples;
    hvk_tex *t;
} hvk_view;
// The host texture of a texture object (tex_t snapshots carry its serial): made on first use, its content uploaded from guest memory when the CPU writes it (a packaged layout,
// be_tex_cpu_written) and again when those pages change, zero otherwise. NULL for a texture the backend cannot make (logged)
hvk_tex *hvk_tex_get(const tex_t *t);
// A view of t: kind HVK_VIEW_*, v the NVN view (NULL = the whole texture), cls_want the numeric class a shader declares for a sampled or storage view (-1 = the format's)
int hvk_tex_view(hvk_tex *t, const view_t *v, int kind, hvk_view *out);
int hvk_tex_is_buffer(const tex_t *t);                     // a buffer texture (target BUFFER): a texel buffer over the pool, no image
VkBufferView hvk_tex_buffer_view(const tex_t *t, int storage, uint8_t *cls);   // its texel buffer view (made on first use), VK_NULL_HANDLE when it cannot be made
VkImage hvk_tex_image(hvk_tex *t);
void hvk_tex_gpu_written(hvk_tex *t);                      // the GPU writes it (attachment, copy, clear, storage image): its content is the GPU's from now on, never uploaded again
int hvk_tex_info(hvk_tex *t, VkFormat *fmt, VkImageAspectFlags *aspect, uint32_t *w, uint32_t *h, uint32_t *depth, uint32_t *layers, uint32_t *levels, VkSampleCountFlagBits *samples, uint8_t *cls);
const tex_t *hvk_tex_desc(hvk_tex *t);
// The registry (be_reg_tex / be_reg_image / be_reg_samp): what a texture ID and a sampler ID name. Caller holds hvk.tex_mu
int hvk_reg_texture(int tid, tex_t *t, view_t *v, int *has_view);   // 1 when tid names a live registration (*t, *v its snapshot)
VkSampler hvk_reg_sampler(int sid, uint8_t cls);           // the sampler for a texture of numeric class cls (integer borders for integer classes); the default sampler for an unregistered ID
// Dummies for unbound slots: a zero texture of the type and class a shader declares (enum nvn_dtt, NVN_DPF_*), a zero texel buffer, a zero storage image
VkImageView hvk_dummy_view(int dtt, int dpf, int storage);
VkBufferView hvk_dummy_texel_buffer(int dpf, int storage);
VkSampler hvk_default_sampler(void);
void hvk_tex_shutdown(void);
// The uploads and zero-fills of textures made or changed in this submission go into the submission's prologue command buffer (vk_commands.c hvk_pre), which runs first
void hvk_tex_frame_begin(void);
// A CPU read of a texture (be_read_texels)
void hvk_tex_read(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img);

// ---------- Shaders (vk_shader.c; design 2.3, 2.6) ----------
// One binding of a shader as the pipeline layout sees it: one descriptor (a combined texture and its sampler are one binding with two reflection entries)
typedef struct {
    uint32_t set, binding, count;
    VkDescriptorType type;
    int16_t rb_tex, rb_samp;   // index into the shader's nvn_refl.b of the texture / buffer entry and of the sampler entry, -1 for none
    uint8_t dim;               // VkImageViewType of a sampled or storage image (from the reflection's dim / arrayed)
    uint8_t cls;               // the numeric class the shader declares: NVN_VK_FLOAT / UINT / SINT
    uint8_t rw;                // a storage buffer or image the shader writes
} hvk_binding;
enum { HVK_MAXB = NVN_REFL_MAXB };
typedef struct hvk_shader {
    char key[24];              // the content key (16 hex digits), or the built-in kernel's name
    int stage;                 // NVN stage: 0 vertex, 1 fragment, 2 geometry, 3 tess control, 4 tess evaluation, 5 compute
    VkShaderModule module;
    char entry[16], entry_u[16], entry_m[16];   // entry points: main, and those for integer colour attachments (dmsh_build.py fs_uint)
    uint8_t mask_m;            // the colour outputs main_m writes as unsigned integers
    nvn_refl rf;               // the neutral reflection (nvn_refl_from_json_text)
    int nb; hvk_binding b[HVK_MAXB];
    VkBuffer c1buf; VkDeviceMemory c1mem;   // the shader's own constants (c1), VK_NULL_HANDLE when it has none
    uint32_t vin_types[32];    // per vertex input location: NVN_CT_* + 1 (0 = not read)
    uint8_t writes_storage;    // a writable storage buffer or image binding
    uint8_t builtin;           // one of the backend's own kernels (nvn_builtin.h's counter kernels)
    uint8_t xfb;               // the module declares the Xfb execution mode: it captures transform feedback
} hvk_shader;
typedef struct hvk_prog { hvk_shader *sh[6]; uint64_t serial; } hvk_prog;
// The program's shaders, per NVN stage: loaded on first use from the shader map and the library (hvk_shader_dir), NULL with a red line for one that is missing. Submit thread
hvk_prog *hvk_prog_get(const prog_t *p);
void hvk_prog_drop(uint64_t serial);               // be_prog_release
uint64_t hvk_prog_compose(uint64_t old_serial, const prog_t *const bst[5]);   // nvn_state_hook_compose
void hvk_shader_shutdown(void);
const char *hvk_shader_dir(void);                  // the library directory: <DM_MSL_DIR or the default cache>/vk-<DMSH_CHAIN_VK>
// A shader by its content key (16 hex digits), its stage from the shader map: what a prewarm row names (any thread; the loads are serialized). NULL when the map does not have
// it (*stale set: a row of another game version, dropped quietly) or it fails to load (a red line, as for a draw)
hvk_shader *hvk_shader_by_key(const char *key, int *stale);
const char *hvk_shader_map_path(void);             // DM_NVN_SHMAP: the prewarm lists are found beside it
int hvk_shader_map_each(void (*fn)(const char *key, int stage, void *u), void *u);   // every shader of the map (the prebuild): how many
// The backend's own SPIR-V (dm/gfx/hostvk/shaders/gen): a module made from a committed array
VkShaderModule hvk_module(const uint32_t *code, size_t bytes);

// ---------- Pipelines and descriptors (vk_pipeline.c; design 2.3, 3.1) ----------
enum { HVK_LAYOUT_VSFS, HVK_LAYOUT_GS, HVK_LAYOUT_TESS, HVK_LAYOUT_CS, HVK_LAYOUT_N };
typedef struct hvk_layout hvk_layout;
// The pipeline layout for the shaders in sh (by NVN stage): the universal one of its stage combination when every binding fits it, else one made from the shaders' own bindings
hvk_layout *hvk_layout_for(hvk_shader *const sh[6]);
VkPipelineLayout hvk_layout_vk(const hvk_layout *l);
// What a graphics pipeline is made from, beyond the shaders: the attachment formats, the topology class and the state the device cannot set dynamically (design 3.1)
typedef struct {
    VkFormat color[8]; VkFormat depth, stencil;
    VkSampleCountFlagBits samples;
    uint8_t topology_class;    // VkPrimitiveTopology of the class (points, lines, triangles, patches), dynamic within it; HVK_TOPOLOGY_ANY (vk_pso.h) where one pipeline draws any class
    uint8_t ccount;            // colour attachments
    uint8_t entry[1];          // the fragment entry chosen (0 main, 1 main_u, 2 main_m)
    uint8_t depth_clip_ndc;    // depth clip control negativeOneToOne (the GL depth mode)
    uint32_t patch_points;
    // the parts of the state baked in when the device cannot set them (zero otherwise): blend, write masks, logic op, polygon mode, depth clamp, alpha to coverage, samples
    uint32_t baked;            // HVK_BAKE_* bits saying which groups below are baked
    VkColorComponentFlags wmask[8];
    VkPipelineColorBlendAttachmentState blend[8];
    uint8_t logic_en, logic_op, poly_mode, depth_clamp, a2c;
    // vertex input when the device has no dynamic vertex input
    uint8_t nvb, nva;
    VkVertexInputBindingDescription vb[16];
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDivisorDescriptionKHR vd[16]; uint8_t nvd;
} hvk_gfx_key;
enum { HVK_BAKE_BLEND = 1, HVK_BAKE_RASTER = 2, HVK_BAKE_VERTEX = 4, HVK_BAKE_LOGIC_OP = 8, HVK_BAKE_PATCH = 16 };
// Graphics pipelines: vk_pso.h (vk_gpl.c, phase P5). Compute pipelines by their shader and layout (any thread)
VkPipeline hvk_cs_pipeline(hvk_shader *cs, hvk_layout *l);
// The dynamic states of a graphics pipeline made whole (a monolithic one: every state the device sets dynamically at the pipeline level, vk_gpl.c)
uint32_t hvk_dyn_states(VkDynamicState *d, int vertex_dynamic, int colors, int bindings);
// The descriptor sets of a draw or dispatch from the resolved state (S, the resolver), cached by content per submission slot, and bound with the driver banks' dynamic offsets.
// base_instance: c0 + 0x644; dispatch: the workgroup counts (c0 + 0x200 of compute). 0 when a set could not be made (logged): the caller skips the draw
int hvk_bind_resources(VkCommandBuffer cb, hvk_layout *l, hvk_shader *const sh[6], VkPipelineBindPoint bp, uint32_t base_instance, const uint32_t dispatch[3]);
void hvk_pipeline_slot_reset(int slot);            // the slot's descriptor pools and set cache (its submission completed)
void hvk_pipeline_shutdown(void);
// The backend's own pipelines (vk_pipeline.c): one compute shader with push constants and buffer device addresses
typedef enum { HVK_K_REPORT, HVK_K_PREDICATE, HVK_K_QUAD, HVK_K_N } hvk_kernel;
int hvk_kernel_dispatch(VkCommandBuffer cb, hvk_kernel k, const void *push, uint32_t push_bytes, uint32_t groups);
// The clear triangle (2.7): writes value into colour attachment slot (mask: channels) or the depth / stencil (slot -1), inside the open render pass with the pass's formats
int hvk_clear_tri(VkCommandBuffer cb, const VkFormat color[8], int ncolor, VkFormat depth, VkFormat stencil, VkSampleCountFlagBits samples, int slot, uint8_t kind,
                  uint32_t mask, const uint32_t value[4], float z, int write_depth, uint32_t stencil_mask, uint32_t stencil_ref, VkRect2D rect, VkExtent2D extent);
// Descriptor microbenchmark access (dm/gfx/hostvk/tests/t_vk_descbench.c): counters of the set cache
void hvk_desc_stats(uint64_t *hits, uint64_t *misses, uint64_t *writes);

// ---------- Commands (vk_commands.c; design 4.5-4.9) ----------
VkCommandBuffer hvk_pre(void);        // the open submission's prologue (uploads, zero-fills), begun on first use
VkCommandBuffer hvk_main(void);       // its command buffer
void hvk_pass_end(void);              // end the open render pass, if any (pending clears land first)
void hvk_need_barrier(void);          // something was written: the next transfer, dispatch or pass begins after a full barrier (design 4.9)
void hvk_barrier_if_needed(VkCommandBuffer cb);
void hvk_full_barrier(VkCommandBuffer cb);
uint64_t hvk_open_fence(void);        // the fence value the open submission commits with (0 when none is open)
uint64_t hvk_done_fence(void);        // the last fence whose CPU work has run
int hvk_slot_index(void);             // the open submission's slot
void hvk_commands_shutdown(void);
int hvk_commands_init(void);

// ---------- The backend's state ----------
typedef struct {
    int on;                    // the device is up: every be_* works on it (0: the null forms of vk_backend.c)
    hostvk_dev dev;
    VkDevice d;
    VkPhysicalDeviceLimits lim;
    pthread_mutex_t mu;        // the pool table and the pools' objects (nvn_pools.h)
    pthread_mutex_t tex_mu;    // textures, registries, samplers
    pthread_mutex_t queue_mu;  // the VkQueue
    _Atomic int origin_ll;     // the device's window origin (be_set_origin_lower_left): the viewport's direction
    int queue_origin_ll;       // the submitting queue's raster origin (be_set_queue_origin_lower_left): the winding
    _Atomic int depth_zero;    // the depth mode (be_set_depth_zero): near = 0 (D3D) or -w (GL)
    _Atomic int tex_pool_n;    // texture IDs past this are empty slots (be_set_tex_pool_n)
    uint64_t frame;            // be_set_frame
    // statistics (be_stats and the like)
    _Atomic uint64_t st_passes, st_draws, st_dispatches, st_pipelines, st_pipeline_ns, st_uploads, st_reuploads, st_upload_bytes, st_upload_ns;   // st_pipelines: made on the submit thread
    _Atomic uint64_t st_shaders, st_barriers, st_pass_splits;
    _Atomic uint64_t st_vcopies_gpu, st_index_gpu;   // unaligned vertex streams copied and quad indices expanded on the GPU (their source not readable on the CPU at encode time)
} hvk_t;
extern hvk_t hvk;
int hvk_up(void);                     // the device is up, initializing it on the first call (pthread_once): 0 = the null forms
// What the frontend's host provides (dm/host): its pre-write hook, before the backend writes guest memory from the CPU (events, counter reports)
void dm_guest_write_prepare(void *addr, size_t len);
uint64_t guest_ns(void);              // the guest clock (counter timestamps are on it)
