// Pipelines without stutter (docs/design/vulkan-backend.md section 3, phase P5): what vk_gpl.c (keys, libraries, fast link, background optimization, monolithic pipelines,
// the waits, the worker threads), vk_cache.c (the VkPipelineCache on disk) and vk_prewarm.c (the prewarm lists, the prediction from their rows, the prebuild) share with
// vk_commands.c (the draw) and vk_backend.c (bring-up, statistics). Its own header, because it needs the neutral pipeline description (nvn_pipe.h) that the other backend files do not.
//
// Threads (design 3.5). The submit thread builds what a draw needs and waits for it (demand first: a draw never waits behind background work). Background workers, below normal
// priority: the prewarm and the speculative builds (one worker on a CPU of up to 4 hardware threads, two above; at most two speculative builds outstanding), and one worker for the
// link-time-optimized pipelines, which also writes the disk cache. During the prepare step (the prebuild of decision D3) one worker per hardware thread less one, at normal priority,
// since the game waits. Every thread compiles into its own VkPipelineCache (vk_cache.c), so the cache writer never reads a cache object a compile is using.
#pragma once
#include "vk_backend.h"
#include "../nvn/nvn_pipe.h"

// ---------- Keys (vk_gpl.c, design 3.1) ----------
// The vertex input a draw's vertex shader reads, in vkCmdSetVertexInputEXT's form: from S's attributes and streams (a draw), or a prewarm row's (bound: bit s for each stream with a
// buffer; an attribute whose stream has none reads the zero buffer, binding 16, as on an unbound Maxwell attribute; a row takes every stream it names as bound)
typedef struct { VkVertexInputBindingDescription2EXT b[17]; VkVertexInputAttributeDescription2EXT a[32]; uint32_t nb, na; uint32_t used_streams; int zero; } hvk_vin;
void hvk_vertex_input(const hvk_shader *vs, const va_st attr[16], const vs_st stream[16], uint32_t bound, hvk_vin *v);
// What a graphics pipeline key is made from besides its shaders: the pass's attachments and the state the device may not set dynamically. A draw fills it from S and its pass,
// a prewarm row from its nvn_pipe_desc; both go through hvk_key_fill, so a row names the very pipeline the draw asks for
typedef struct {
    int nc; VkFormat cfmt[8]; uint8_t ccls[8]; VkFormat dfmt, sfmt; VkSampleCountFlagBits samples;
    const blend_st *blend; uint32_t blend_en, chmask, logic_op, poly_mode; int depth_clamp, a2c;   // NVN values (blend: 8 targets)
    const hvk_vin *vin;
} hvk_key_src;
// The key (design 3.1): the attachment formats, the entry point, the depth mode, the topology class (left out where the device draws any class from one pipeline), and what the
// device cannot set dynamically at the pipeline level (hvk_pso_level: the device's, or DM_VK_LEVEL's). k is zeroed first, so keys compare with memcmp
void hvk_key_fill(hvk_gfx_key *k, hvk_shader *const sh[6], int klass, const hvk_key_src *s);
// One colour attachment's blend state as Vulkan takes it: blending off on a format that cannot blend (t_vk_integer_blend), the factors of MIN / MAX equations ONE
void hvk_blend_of(const blend_st *b, int on, VkFormat f, uint32_t wmask4, uint32_t *en, VkColorBlendEquationEXT *eq, VkColorComponentFlags *wm);
// The attachment format of NVN format nf as vk_texture.c makes an image of it (the table's, or its substitute where the device lacks the use), and its numeric class (vk_texture.c)
VkFormat hvk_attach_format(int nf, uint8_t *cls);
#define HVK_TOPOLOGY_ANY 0xff   // hvk_gfx_key.topology_class when the device draws every class from one pipeline (dynamicPrimitiveTopologyUnrestricted)

// ---------- The pipeline store (vk_gpl.c, design 3.2, 3.5, 3.6) ----------
void hvk_pso_init(void);       // at bring-up, after the device: the level's rules, the device's quirks, DM_VK_PSO_TRACE
void hvk_pso_shutdown(void);   // the workers stopped, every pipeline and library destroyed
// The level pipelines are made for: the device's (or DM_VK_LEVEL's), C where the libraries are present but cannot be used as design 3.2 needs them (no fast linking, or no
// independent interpolation decorations: the interpolation-matched vertex variants are not built yet)
char hvk_pso_level(void);
int hvk_pso_gpl(void);         // 1: pipelines are linked from libraries (levels A and B)
// The draw's pipeline (submit thread). A hit costs nothing; a miss is built here, at once (A / B: the libraries that are missing, then a fast link; C / D: the monolithic
// pipeline), or waited for when a worker is building it, and the wait is counted per reason with its time (be_pso_line; DM_VK_PSO_TRACE=1 logs each one with its key).
// An optimized pipeline the background built for the key is swapped in here; the fast-linked one is released after the submissions that may use it. miss: called on a miss
// that no list named (the user list's P row, vk_prewarm.c); NULL for none. VK_NULL_HANDLE with a red line when the pipeline cannot be made
VkPipeline hvk_gfx_pipeline(hvk_shader *const sh[6], hvk_layout *l, const hvk_gfx_key *k, void (*miss)(void));
// The prewarm's builds (any thread): the pipeline of the key, made on the calling thread if no one has it (row: the list row that names it, for the prediction; -1 none).
// 1 made or present, 0 failed
int hvk_pso_build(hvk_shader *const sh[6], hvk_layout *l, const hvk_gfx_key *k, int row);
// An S row's libraries (A / B): the pre-rasterization library of a vertex shader, or the fragment-shader library of a fragment shader, for its universal layout and the default
// entry point. 1 made or present, 0 failed or not a stage with a library
int hvk_pso_build_shader(hvk_shader *sh);
// Whether the key's pipeline is ready (a draw of it would not wait)
int hvk_pso_ready(hvk_shader *const sh[6], hvk_layout *l, const hvk_gfx_key *k);
// Background work: fn(arg) on a worker. HVK_Q_BG: the prewarm and speculative builds (list order; HVK_Q_SPEC first); HVK_Q_OPT: link-time-optimized relinks, run only while
// the other queues are empty
enum { HVK_Q_SPEC, HVK_Q_BG, HVK_Q_OPT, HVK_Q_N };
void hvk_work_submit(int q, void (*fn)(void *), void *arg);
int hvk_work_pending(int q);   // queued or running
// The prepare step (vk_prewarm.c): workers at normal priority until hvk_work_prepare_end, so that with the background ones there are `total`; they take HVK_Q_SPEC and HVK_Q_BG
// work. Returns how many it made
int hvk_work_prepare_begin(int total);
void hvk_work_prepare_end(void);
void hvk_work_drain(int q);    // wait until nothing of that queue is queued or running (HVK_Q_N: all)

// Statistics of the pipelines (be_pso_line, the tests)
enum { HVK_W_LINK, HVK_W_LIB, HVK_W_MONO, HVK_W_INFLIGHT, HVK_W_N };   // why a draw waited
extern const char *const hvk_wait_why[HVK_W_N];
typedef struct {
    uint64_t lib[4], lib_ns[4];             // libraries made: vertex input, pre-rasterization, fragment shader, fragment output
    uint64_t fast, fast_ns, opt, opt_ns;    // fast links, optimized links (all threads)
    uint64_t mono, mono_ns;                 // monolithic pipelines (levels C, D)
    uint64_t swapped, opt_dropped;          // optimized pipelines swapped in at a draw; optimized relinks dropped (quirk, test switch, failure)
    uint64_t draw_made, draw_ns;            // made on the submit thread for a draw (its wait)
    uint64_t wait_n[HVK_W_N], wait_ns[HVK_W_N], wait_max_ns, over_budget;   // waits per reason; the longest; waits over 2 ms (the P5 acceptance)
    uint64_t draws_fast, draws_opt, draws_mono;   // draws by the kind of pipeline they bound
    uint64_t failed;
    uint64_t pending;                       // keys being built now
} hvk_pso_stats;
void hvk_pso_stats_get(hvk_pso_stats *s);
uint64_t hvk_pso_take_window_max(void);   // the longest wait since the last call (be_pso_line's window)
// The per-vendor quirk table (design 3.2, risk 7, 11.2 item 10: in the backend, never a game profile key): the flags for this device and the table's row that gave them
enum { HVK_QUIRK_NO_OPT = 1 };   // the optimized relink does not give the fast link's pixels: the fast-linked pipeline stays
uint32_t hvk_quirks(const char **why);
// Test hooks (dm/gfx/tests/vk): make pipelines for that level from now on (every queue drained, every pipeline and library dropped first; the device keeps what it enabled), and
// turn the background optimization on or off
void hvk_pso_test_level(char level);
void hvk_pso_test_optimize(int on);

// ---------- The disk cache (vk_cache.c, design 3.4) ----------
void hvk_cache_init(void);       // at bring-up: the folder, files of other versions removed, this version's file read and checked
void hvk_cache_shutdown(void);   // written a last time, every cache object destroyed
// The calling thread's VkPipelineCache, held against the writer until hvk_cache_end (made on its first use from the file's data); VK_NULL_HANDLE with the cache off
VkPipelineCache hvk_cache_begin(void);
// The creation is over: fb the creation feedback it asked for (NULL: none), made 1 when a pipeline or library was created (the burst timer of the periodic write)
void hvk_cache_end(const VkPipelineCreationFeedback *fb, int made);
void hvk_cache_retire_thread(void);   // a thread that ends (a prepare worker): its cache merged into the shared one and destroyed
int hvk_cache_flush(const char *why); // merge every thread's cache and write the file now (copy and rename): 1 written, 0 unchanged or off, -1 failed (logged)
void hvk_cache_tick(uint64_t now);    // the optimizing worker, about once a second: writes 10 s after the last of a burst of new pipelines
typedef struct { uint64_t loaded, written, writes, created, hits; int on; char path[1024]; } hvk_cache_stats;   // bytes of driver data read and written; creations and driver cache hits
void hvk_cache_stats_get(hvk_cache_stats *s);

// ---------- Prewarm, prediction, prebuild (vk_prewarm.c, design 3.3) ----------
void hvk_prewarm_start(void);    // at the first submission (the depth mode is the game's by then): the lists read, their rows queued; with DM_VK_PREBUILD=1 it returns when all are built
void hvk_prewarm_shutdown(void);
void hvk_prewarm_predict(int row);   // a draw used the pipeline of list row `row`: the rows recorded after it are built next (at most two speculative builds outstanding)
void hvk_prewarm_predict_pair(const char *vs, const char *fs);   // a draw missed with these shaders: their pair's rows (and the next pair's) are built next, likewise
void hvk_prewarm_user_row(const nvn_pipe_desc *d);   // a pipeline built on the spot that no list named: appended to the device's user list as a P row
int hvk_prewarm_done(void);      // the lists' rows have all been built (or there were none)
typedef struct { uint64_t lines, rows, shader_rows, compute_rows, stale, skipped, built, failed, speculative, user_rows; double seconds; int prebuild, running; } hvk_prewarm_stats;
void hvk_prewarm_stats_get(hvk_prewarm_stats *s);
void hvk_prewarm_test_show_after(uint64_t ns);   // tests: the prepare step's progress screen shows from this time on (default 0.5 s, the Metal backend's rule)
