# Vulkan graphics backend for Windows and Linux (design)

Status: proposal, not implemented. Written on 2026-10-28 against branch `multiplataforma` at commit `8ed424a`. Every line number below refers to that commit.

Review: reviewed on 2026-10-28 on Linux (the owner's Ubuntu PC, a checkout with LF line endings) by reading sections 0 to 13 against the repository at `8ed424a`. Verified: over 60 repository claims marked [V] in sections 1 to 13 (file and line citations, the profile-key counts of 5.6, the four `feedback_snap` and four `render_cond` profiles, the 14 run-time MSL compiles, the games table's 84 NVN, 3 Vulkan and 2 OpenGL games, the manifest row counts, the cited reference entries); three were corrected (the function counts of 1.1, the Monster Hunter Rise count in 2.3 and section 6). On this checkout `python3 dm/gfx/tools/dmsh_chain.py` prints the committed `157e1b96c8da`, which confirms that the different version of 2.5 comes from CRLF line endings. Not verified: the external figures (gpuinfo.org shares, driver release notes, Mesamatrix), every [A], and anything that needs a GPU, a Mac or the games; no code was built or run.

Steps M0 and M1 (2026-10-28, on Linux): M0 as in 1.7, with the tool fingerprints off a Mac in `dm/gfx/tools/chain_tools.py` (an edit of `dmsh_chain.py` itself would change `DMSH_CHAIN`). M1 adds `dm/gfx/nvn/nvn_be.h`, through which `hle_nvn.c` makes all its backend calls (on Apple a one-line forward to each `mtl_*`, `be_pool_add` with the pool's flags); not yet compiled on macOS. Left: `be_report` (the statistics stay one `be_*` per `mtl_*` until a Mac can compare the logs), `be_get_caps` and `be_guest_map` / `be_guest_unmap` for the Vulkan backend, the per-operation group for M3, and for M6 the guest Vulkan and GL layers and the host files that still call `mtl_*` (`mtl_device_classic` and three others have no `be_*` yet).

Step M2, state and ledger (2026-10-29, on Linux): `S`, `R`, `state_op` (now `nvn_state_apply`, which returns a dirty mask), `bind_stages`, `prog_resolve`, `state_reset` and the per-queue state are in `dm/gfx/nvn/nvn_state.c`, the GPU-write ledger (`pend_*`, `sbw_*`, `gw_*`) in `nvn_gpuwrite.c`; `nvn_mtl.m` keeps the old names as wrappers, turns the mask into its flags, and keeps four hooks (`nvn_state_hook_*`: the combined program's `NvProg`, `bh_fresh` and the pass dump of texture binds, two red lines). CPU tests `t_state` and `t_gpuwrite` run in CTest and the macOS CI. Left for M2b: `nvn_resolve.c` (handle helpers, `bh_fresh`, which a hook still calls) and `nvn_pools.c`; for a Mac: compiling `nvn_mtl.m` (macOS CI) and the checks of 1.7.

Step M2b, handles and pools (2026-10-29, on Linux): `tex_slot_h`, `c2_handle`, `c0_ssbo_base` and the driver segment fill of `encode_args` (`nvn_fill_driver_bank`, which returns what was reported red), `ubo_read`, `ubo_handle`, `ubo_tex_id`, `ubo_samp_id` (`nvn_ubo_read`, `nvn_ubo_handle`, `nvn_tid_from_handle`, `nvn_sid_from_handle`) and `bh_fresh` (`nvn_bind_fresh`) are in `dm/gfx/nvn/nvn_resolve.c`; the pool table, `pool_find`, the per-thread lookup cache and `g_pool_gen` in `nvn_pools.c`, with an opaque backend handle and the NVN flags per pool and no lock of its own (the caller holds the backend's, `g_mu` on Metal, as before). `nvn_mtl.m` keeps the old names as wrappers and five hooks (`nvn_resolve_hook_*`: its own copies of inline updates, red line 47, the memory read through `mtl_addr`; `nvn_pool_hook_*`: residency, the release of a removed pool's buffer). CPU tests `t_resolve` and `t_pools` run in CTest and the macOS CI. Left for M3: `bh_fresh` still runs from the bind hook, `nvn_texreg.c`; for M6 and M7: `be_addr` (`hle_nvn.c` still reaches `mtl_addr`), the pool flags through `be_pool_add` (the Metal backend records 0), the Vulkan backend's lock around the table; for a Mac: compiling `nvn_mtl.m` (macOS CI).

Step M4, first half (2026-10-29, on Linux): the reflection parser in plain C, `nvn_refl_from_json_text` in `dm/gfx/nvn/nvn_refl.c` over vendored cJSON 1.7.19 (`dm/third_party/cjson`), makes the same `nvn_refl` as `nvn_refl.m`, Metal texture type numbers included (`nvn_refl_textype.h`); `t_refl_c` checks it in CTest and the macOS CI, and `t_refl_cmp` (Mac) parses a corpus of about 37,700 documents, and with `DM_REFL_DIR` a build cache's reflection, with both parsers and compares the structs byte for byte. `nvn_mtl.m` still calls `nvn_refl.m`: the switch, the removal of `nvn_refl.m` and the neutral enums come with M6.

Goal: run the games of the [games table](../../dm/games/README.md) on Windows 10 and later and on Linux, drawing through Vulkan, with the Metal backend on Apple platforms unchanged. The user's requirements, which every choice below is checked against:

- zero loss of visual quality relative to what the Switch draws;
- no stutter, and no asynchronous shader scheme that draws a wrong frame (a skipped draw, a placeholder shader) while a pipeline compiles;
- maximum performance on modest hardware too: a Core i3 with an Intel integrated GPU, or an ordinary discrete GPU;
- balanced load times.

The CPU side (an ahead-of-time ARM64 to x86-64 recompiler, a platform layer for the host, SDL3 for the window, input and audio) is designed separately (`docs/design/aot-recompiler.md`, written by another agent). This page names what the graphics side needs from it and nothing more.

**Claim tags.** Each factual claim carries one of two tags:

- **[V]** verified: a repository file and line (`path:line`), a command run on this machine (Windows 11, this checkout), or an external source listed under [Sources](#sources).
- **[A]** assumption: not verified; the text says how to verify it.

Design statements (what we propose to build) carry no tag.

**Boundaries.** Following [CLAUDE.md](../../CLAUDE.md): this design touches the NVN graphics backend boundary (`dm/gfx/nvn`), adds a new host Vulkan backend beside it, and touches the guest Vulkan and OpenGL layers (`dm/gfx/vk`, `dm/gfx/gl`) only for their non-Apple builds. The host (`dm/host`) is touched only through the interfaces listed in section 1.6. No game profile changes.

## 0. Summary

1. **One frontend, two backends, chosen at link time.** The record stream that `dm/gfx/nvn/hle_nvn.c` writes into the game's command memory (`dm/gfx/nvn/nvn_obj.h:98-180`) is already API-neutral. Its replay, `dm/gfx/nvn/nvn_mtl.m` (10,989 lines), mixes NVN semantics with Metal encoding. The decoder, the NVN state model, handle resolution, the pool and texture registries, the in-submission GPU-write ledger, shader and reflection loading and the prewarm lists move into C files of an API-neutral frontend in move-only steps. The Metal-specific lookahead optimizations for tile-based GPUs stay inside the Metal backend. A header `nvn_be.h` declares the backend functions; each host binary links exactly one backend (no virtual table on the draw path).
2. **The Vulkan path consumes the translator's SPIR-V directly.** `dmsh_xlate` already asks Ryujinx's translator for Vulkan SPIR-V (`dm/gfx/tools/dmsh_xlate/Program.cs:165`) [V]. Metal-only steps (two depth entry points, vertex pulling, readable-tile variants, geometry and transform feedback emulation, the one-lane subgroup rewrite) are not needed on Vulkan; Maxwell-semantics steps (output padding, output defaults, integer inputs and outputs, NaN behaviour) get SPIR-V counterparts. A separate translator build and chain version (`DMSH_CHAIN_VK`) keep the Metal chain untouched.
3. **Pipelines in two phases.** Universal pipeline layouts and extended dynamic state reduce a pipeline to its shaders and attachment formats. With `VK_EXT_graphics_pipeline_library`, shader libraries are compiled at load, linked fast at first use, and the optimized pipeline is compiled in the background and swapped in. The `VkPipelineCache` is kept on disk. Without the extension, a draw waits for its exact pipeline (never a wrong frame) and the prewarm lists, the look-ahead prediction and an optional first-start prebuild keep such waits rare.
4. **Guest memory is GPU memory.** Pool storage is imported into Vulkan with `VK_EXT_external_memory_host` (one import per pool, coalesced where a driver limits the number of allocations), so any guest address is a buffer and offset, as on Apple's unified memory: zero-copy and exact on integrated GPUs. On discrete GPUs, ranges the GPU only reads are mirrored into device-local memory, driven by the existing dirty-page tracker. Images are device-local, as Metal's private textures are today.
5. **Counters and queries are written by the GPU, in order.** That makes conditional rendering exact (`VK_EXT_conditional_rendering`), closes the Metal backend's same-frame occlusion-query gap, and makes several Apple-motivated profile keys unnecessary on Vulkan.
6. **Native features.** Geometry and tessellation shaders, transform feedback, logic operations, exact border colours, wide lines, depth bounds, conservative rasterization and sample locations are native on desktop Vulkan.
7. **Textures.** BC formats are native on desktop GPUs. ASTC is native on some Intel integrated GPUs; elsewhere it is decoded on the GPU at upload, in a format the user chooses (section 12).
8. **Presentation** goes through a presenter thread with a vsync source that uses `VK_KHR_present_wait` where present and falls back elsewhere; FIFO by default; raw bytes as the Switch scans them out.
9. **Guest Vulkan becomes a pass-through shim; guest OpenGL needs a user decision.**
10. **Baseline: Vulkan 1.3** with a short list of required extensions; Vulkan 1.4 and the stutter-related extensions are used when present. Continuous integration runs the Vulkan tests on Mesa's software rasterizer (lavapipe) with the validation layers.

## 1. The boundary: NVN frontend and graphics backends

### 1.1 What exists today

- **Interface layer** `dm/gfx/nvn/hle_nvn.c` (2,539 lines) takes over `nvnBootstrapLoader`, implements the NVN entry points, stores host objects in place in the game's NVN object memory and writes command records into the game's command memory ([dm/gfx/README.md](../../dm/gfx/README.md), lines 9-18) [V]. It contains 4 occurrences of `MTL`, all in comments [V: grep], and 33 uses of Mach time APIs [V: grep], which belong to the host platform layer.
- **Record format** `dm/gfx/nvn/nvn_obj.h`: object layouts, the opcode list (`nvn_obj.h:100-108`, 51 opcodes) and payloads [V]. Records hold copies of texture objects (`REC_SNAP`, [rendering.md](../reference/rendering.md), "Texture objects copied into command records") [V], so the replay never dereferences game objects that may have changed.
- **Replay layer** `dm/gfx/nvn/nvn_mtl.m`: at `nvnQueueSubmitCommands` (`hle_nvn.c:1182-1203`) the interface layer calls `mtl_sub_begin`, `mtl_exec` for each command handle and `mtl_sub_commit` under one submission lock [V]. `mtl_exec` (`nvn_mtl.m:9702-9848`) is the decoder: a loop over records that updates the draw state `S` (`nvn_mtl.m:4234-4249`) and render-target state `R` (`nvn_mtl.m:2583-2588`) through `state_op` (`nvn_mtl.m:6734-6799`) and calls `draw`, `dispatch`, copies and clears [V].
- **Replay interface already in C:** 85 distinct functions, 56 declared in `nvn_obj.h:182-248` and 30 in `dm/gfx/nvn/nvn_mtl.h:8-52`, `mtl_present_wait` in both (`mtl_pool_add`, `mtl_exec`, `mtl_present`, `mtl_wait`, `mtl_reg_tex`, …) [V: grep]. `hle_nvn.c` uses 82 distinct `mtl_*` functions (80 called directly, `mtl_wait` and `mtl_wait_acq` through a function pointer), most of them statistics for its periodic log lines [V: grep].
- **Memory model:** pools are zero-copy `MTLBuffer`s over game memory, so the GPU address equals the CPU address and only unified-memory devices are supported (`dm/gfx/README.md:20`; `nvn_mtl.m:360` stops on a device without unified memory; `nvn_mtl.m:487` creates the buffer with `newBufferWithBytesNoCopy`) [V].

### 1.2 Metal coupling, measured

Counts of the text `MTL` (one per occurrence) at `8ed424a` [V: grep]:

| Area | Files | `MTL` occurrences | Nature of the coupling |
|---|---|---|---|
| `dm/gfx/nvn` | 11 of 32 tracked files | 2,563 | almost all in `nvn_mtl.m` (2,327) and `nvn_rec.h` (113) |
| `dm/gfx` without tests | 29 of 162 | 3,406 | adds the guest Vulkan and GL layers (285 and 386), `tex/nvn_deswizzle_gpu.m` and `.h` (17), `metal/` (a standalone sample, not in the host) |
| `dm/gfx/tests` | 58 of 95 | 1,596 | the GPU tests compile production code and link Metal (`dm/gfx/tests/Makefile:18`) |
| `dm/host` | 3 | 8 | `front.h` (`front_metal_layer`, `front.h:15`), `front_mac.m`, `memstat.m` |

Per file of the NVN layer:

| File | Lines | Coupling | Fate |
|---|---|---|---|
| `nvn/hle_nvn.c` | 2,539 | names of the `mtl_*` interface; Mach time (33) | frontend; calls go through `nvn_be.h` |
| `nvn/nvn_obj.h` | 248 | declarations of the replay API | frontend |
| `nvn/nvn_mtl.h` | 52 | replay API (device, pools, waits, external producers) | becomes `nvn_be.h` |
| `nvn/nvn_mtl.m` | 10,989 | 2,327 `MTL`, 848 Foundation `NS*`, 91 `dispatch_*`, 102 `os_unfair_lock`, 67 `CF*`, 19 `@autoreleasepool`, 11 MSL sources compiled at run time [V: grep] | split: roughly 1,500 lines frontend, the rest Metal backend (next table) [A: an estimate from the table] |
| `nvn/nvn_rec.h` | 333 | deferred encoding: stand-ins for Metal encoders (`nvn_rec.h:1-8`) | Metal backend |
| `nvn/nvn_geometry.inc`, `nvn_feedback.inc`, `rt_range_check.inc` | 324, 85, 62 | geometry and transform feedback emulation, a diagnostic | Metal backend |
| `nvn/nvn_refl.m`, `nvn_refl.h` | 155, 62 | JSON parsed with `NSDictionary` (`nvn_refl.h:58-62`); `dtt` holds a `MTLTextureType` value (`nvn_refl.h:28`); `aid` is an argument-buffer index | parser in C, neutral enums |
| `nvn/nvn_pso_skey.h` | 59 | C, but the key's `cfmt` / `dfmt` / `sfmt` hold `MTLPixelFormat` values (`nvn_mtl.m:3279`, `3282`) | neutral pipeline description beside it |
| `nvn/nvn_builtin.h` | 33 | four hand-written MSL kernels (`nvn_builtin.h:15-25`) | GLSL counterparts for Vulkan |
| `nvn/nvn_dirty.c` | 216 | POSIX `mprotect`, Mach time (`nvn_dirty.c:11`, `43-46`) | frontend, through the platform layer |
| `nvn/nvn_tex.c`, `nvn_hmap.h`, `nvn_cover.h`, `nvn_pace.h`, `nvn_fstep.h`, `nvn_primitive.h`, `nvn_timestamp.h`, `nvn_depth_clear.h`, `nvn_geometry.h`, `nvn_feedback.h` | — | none (pure C) | frontend as they are |
| `nvn/nvn_thunks.S`, `nvn_entry.S` | 6,993, 91 | AArch64 assembly (generated thunks) | CPU port (section 1.6) |
| `tex/nvn_fmt_mtl.c` | 83 | pure C table of NVN format → `MTLPixelFormat` value (`nvn_fmt_mtl.c:28-71`) | stays; a Vulkan table beside it; its inverse decodes old manifest rows |
| `tex/nvn_deswizzle.c` | 83 | none | shared |
| `tex/nvn_deswizzle_gpu.m` | 73 | Metal compute kernel | Metal backend; GLSL counterpart for Vulkan |

Inside `nvn_mtl.m`, by section (line ranges from its function map) [V: awk count]:

| Lines | Section | Lines with `MTL` | Frontend share |
|---|---|---|---|
| 1-303 | residency set, deferred release | 26 of 303 | none |
| 304-566 | memory pools, device creation | 35 of 263 | the address → pool index (`pool_find`, `nvn_mtl.m:533`; per-thread cache `pc_get`, `nvn_mtl.m:461`) |
| 567-1536 | submission, post-completion writes, in-submission GPU-write ledger, encoders, uniform shadows, waits | 96 of 970 | post ledger (`post`, `nvn_mtl.m:1201-1206`), GPU-write ledger (`pend_*` 654-686, `sbw_*` 639-653, `gw_*` 1076-1098), render-condition values (`rc_value`, 594) |
| 1537-2611 | texture storage, uploads, sharing table | 143 of 1,075 | the sharing keys and CPU-written texture decisions |
| 2612-3367 | render passes, attachments, profiling | 139 of 756 | none (pass mechanics are per API) |
| 3368-3792 | shader map, shader loading, programs | 29 of 425 | map loading (`shmap_load`, 3375), composite programs (`prog_fold_gs`, 3719) |
| 3793-4233 | texture registry, views, samplers, frame arena | 87 of 441 | the ID → object tables behind `mtl_reg_tex` (3896) and `mtl_reg_samp` (3935) |
| 4234-4479 | draw state, vertex buffers, per-stage program binds, queue state | 8 of 246 | nearly all: `S`, `bind_stages` (4408), `prog_resolve` (4433), `state_reset` (4439), `mtl_queue_activate` / `forget` (4454, 4467) |
| 4480-5372 | vertex formats, blend factors, pipeline descriptors, Metal 4 archive | 128 of 893 | none |
| 5373-6050 | pipeline keys, precompile prediction, hybrid pipelines, prewarm | 54 of 678 | manifest parsing, the prediction's state replay (`pso_predict_rest_`, 6853) |
| 6051-6533 | dummies, binding encoding | 130 of 483 | handle helpers (`tex_slot_h` 46, `c2_handle` 4251, `ubo_read` / `ubo_handle` / `ubo_tex_id` / `ubo_samp_id` 6259-6276, `bh_fresh` 3946) |
| 6534-8661 | lookahead optimizations, `state_op`, feedback snapshots, indirect conversion, lazy clears, dispatch hoisting | 125 of 2,128 | `state_op` (6734-6799, no `MTL` at all); the rest stays Metal |
| 8662-9080 | draw, dispatch | 48 of 419 | none |
| 9081-9701 | copies, clears | 145 of 621 | none |
| 9702-9849 | decoder `mtl_exec` | 2 of 148 | all of it |
| 9850-10988 | present, display link, diagnostics | 161 of 1,140 | none |

The lookahead optimizations (pass merging and store elision for tile-based GPUs, slice merging, clear hoisting, dispatch hoisting, deferred draws) read the record stream directly and are motivated by Apple GPU costs ("a render pass costs about 35-40 µs", [rendering.md](../reference/rendering.md), "Passes into other layers of one texture level run as one layered pass") [V]. They stay inside the Metal backend. Only what a second backend needs moves.

### 1.3 Target structure

```
guest NVN calls
   │
dm/gfx/nvn/hle_nvn.c            interface layer (unchanged role): objects in place, records into command memory
   │  QueueSubmitCommands
dm/gfx/nvn/nvn_replay.c         NEW frontend decoder: walks records (JUMP / CALL / END), applies state ops,
   │                            resolves handles, keeps the GPU-write and post-completion ledgers
   │  be_* calls (nvn_be.h), in record order
   ├── dm/gfx/nvn/nvn_mtl.m     Metal backend (Apple): today's case bodies, verbatim, plus its lookahead
   └── dm/gfx/hostvk/*.c        Vulkan backend (Windows, Linux): device, memory, textures, pipelines, encoding, presenter
                                also the shared device and presenter of the guest Vulkan and GL layers on those hosts
```

The new directory is named `hostvk`, not `vk`: `dm/gfx/vk` already means "the guest's Vulkan API" ([dm/gfx/README.md](../../dm/gfx/README.md), line 84) [V].

Frontend files (new, pure C11, compiled on every host):

| File | Content, and where it comes from |
|---|---|
| `nvn/nvn_state.c`, `.h` | `S`, `R`, `state_op` (`nvn_mtl.m:6734`), `bind_stages`, `prog_resolve`, `state_reset`, per-queue save and restore. `state_op` returns a dirty mask instead of setting Metal's `g_pso_dirty`, `g_dss_dirty`, `g_args_dirty[]` |
| `nvn/nvn_replay.c` | the decode loop of `mtl_exec` (`nvn_mtl.m:9702-9848`) and the record cursor used by look-ahead scans (JUMP, CALL, return stack) |
| `nvn/nvn_resolve.c` | handle resolution: `tex_slot_h`, `c2_handle`, `ubo_handle` with `pend_read` for queued inline updates, pool bases, deferred binds (`bh_fresh`), the driver banks c0 / c2 contents |
| `nvn/nvn_gpuwrite.c` | the ledger of GPU writes queued in the submission and still running (`pend_*`, `sbw_*`, `gw_*`), which both backends need for barriers, CPU reads and uniform shadows |
| `nvn/nvn_pools.c` | pool table and address lookup with an opaque backend handle per pool |
| `nvn/nvn_texreg.c` | texture and sampler ID tables, serial tables, sharing keys, CPU-written texture state |
| `nvn/nvn_post.c` | post-completion ledger (events, counter reports done on the CPU, fence publication, fast wake) |
| `nvn/nvn_shaders.c` | shader map, reflection parser in C (replacing `nvn_refl.m`), shader key to stage |
| `nvn/nvn_pipe.c` | neutral pipeline description (NVN formats and NVN state), prewarm manifest rows: decodes today's `R` rows through the inverse of `nvn_fmt_to_mtl` and writes new portable `P` rows |

### 1.4 What stays in the Metal backend

Everything that creates or encodes Metal objects, and every optimization motivated by Apple GPUs: residency sets, the zero-copy pool buffers, private textures and their uploads, argument buffers, Metal 4 archives, the hybrid pulling stand-ins, readable-tile pass merging, store elision, slice merging, the display link and MetalFX. The Metal backend keeps its own fast paths (for example `tex_fast_rid` in `encode_args_`, `nvn_mtl.m:6283`) and may adopt a shared frontend helper later only when a measurement shows no cost (the project rule: no abstraction until a second caller needs it).

### 1.5 The backend interface

`dm/gfx/nvn/nvn_be.h`, sketched. The first group is today's `mtl_*` API renamed; the per-operation group appears in migration step M3 (section 1.7) and receives exactly what today's `case` arms of `mtl_exec` receive.

```c
/* nvn_be.h: one implementation per host binary (nvn_mtl.m on Apple, dm/gfx/hostvk elsewhere) */
#pragma once
#include "nvn_obj.h"

/* device and capabilities */
void be_init(void);                                         /* was mtl_init */
typedef struct {
    uint32_t unified_memory : 1, gpl : 1, eds3_blend : 1, eds3_raster : 1, vertex_input_dynamic : 1,
             cond_render : 1, xfb : 1, geometry : 1, tessellation : 1, astc_ldr : 1, logic_op : 1;
    uint32_t max_samples, max_point_size, max_vertex_attribs;
} be_caps;
const be_caps *be_get_caps(void);

/* memory pools: the guest storage and the pool's NVN flags (a new argument; Metal ignores it) */
void be_pool_add(const void *mem, uint64_t size, uint32_t nvn_flags);
void be_pool_del(const void *mem);
void be_alias_drop_pool(const void *pool);
void be_guest_map(void *mem, uint64_t size);               /* new: guest memory committed (hle_svc.c) */
void be_guest_unmap(void *mem, uint64_t size);             /* new: before it is decommitted */

/* object hooks: the frontend owns the tables, the backend its GPU objects */
void be_tex_init(const tex_t *t);
void be_tex_release(tex_t *t);
void be_tex_cpu_written(const tex_t *t);
int  be_tex_gpu_owned(uint64_t serial);
void be_reg_tex(int id, const tex_t *t, const view_t *v);
void be_reg_image(int id, const tex_t *t, const view_t *v);
void be_reg_samp(int id, const samp_par *p);
void be_prog_release(prog_t *p);
void be_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r,
                    uint8_t *dst, size_t row, size_t img);
void be_set_depth_zero(int on);
void be_set_origin_lower_left(int on);
void be_set_queue_origin_lower_left(int on);

/* submission and sync (hle_nvn.c, under its submission lock) */
void     be_queue_activate(uint64_t queue_serial);
void     be_queue_forget(uint64_t queue_serial);
void     be_sub_begin(void);
void     be_sub_index(const uint64_t *handles, int n);      /* Metal: store look-ahead index; Vulkan may ignore it */
uint64_t be_sub_commit(void);                               /* returns the fence value of this submission */
uint64_t be_last_fence(void);
int      be_wait(uint64_t fence, uint64_t timeout_ns);      /* 1 = completed, 0 = timeout */
int      be_wait_acq(uint64_t fence, uint64_t timeout_ns);
void     be_finish(void);
void     be_drain(void);

/* per operation, called by nvn_replay_exec() in record order (step M3) */
int  be_rec_pre(const uint8_t *rec, const op_hdr *h);       /* Metal look-ahead hooks; nonzero: record consumed */
void be_state_dirty(uint32_t mask);                          /* NVN_DIRTY_PIPELINE, NVN_DIRTY_DEPTH_STENCIL, NVN_DIRTY_ARGS(stage) */
void be_set_rt(const p_set_rt *r, const uint8_t *rec);
void be_clear_color(const p_clear_color *c);
void be_clear_ds(const p_clear_ds *c);
void be_draw(const p_draw *dw, const p_draw_idx *di, const uint8_t *rec, const uint8_t *next);
void be_dispatch(const p_dispatch *dp, const uint8_t *rec);
void be_copy_b2b(uint64_t src, uint64_t dst, uint64_t n);
void be_update_ub(const void *data, uint64_t dst, uint64_t n, const uint8_t *rec);
void be_fill(uint64_t dst, uint64_t n, uint32_t value);
void be_copy_b2t(const p_copy_b2t *c);
void be_copy_t2b(const p_copy_b2t *c);
void be_copy_t2t(const p_copy_t2t *c);
void be_clear_tex(const p_clear_tex *c);
void be_barrier(int32_t nvn_barrier_bits);
void be_report_counter(uint64_t addr, uint32_t nvn_counter_type);
void be_reset_counter(uint32_t nvn_counter_type);
void be_signal_event(volatile uint32_t *val, uint32_t mode, uint32_t value);
void be_fence_sync(sync_t *s);
void be_render_cond(const p_render_cond *rc);
void be_tf_state(const p_tf_state *b);

/* presentation and pacing (hle_nvn.c acquire / present, guest VK and GL layers) */
uint64_t be_present(const tex_t *t, double period_s, const int crop[4]);
void     be_present_wait(uint64_t ticket);
void     be_present_flush(void);
int      be_has_window(void);
int      be_display_hz(void);
int      be_vsync_active(void);                              /* was mtl_link_active */
int      be_vsync_wait(int vsyncs, uint64_t timeout_ns);     /* was mtl_link_wait_tick */
int      be_vsync_wait_ticks(int ticks, uint64_t timeout_ns);
int      be_vsync_rate(void);
int      be_vsync_stalled(void);
double   be_presented_time(uint64_t n);
int      be_fps_target(void);

/* external producers: the guest Vulkan and GL layers on the same host */
void    *be_native_device(void);                              /* Metal: id<MTLDevice>; Vulkan: the shared device record */
uint64_t be_ext_tex(void *native_image);
uint64_t be_present_ext(uint64_t serial, double period, void *event, uint64_t event_value, const int crop[4]);

/* frames and statistics: the backend formats its own log lines */
void   be_set_frame(uint64_t frame);
double be_gpu_ms(uint64_t frame);
int    be_gpu_span(uint64_t frame, uint64_t *t0, uint64_t *t1);
void   be_report(uint32_t tick, char *out, size_t n);        /* replaces about 40 mtl_*_stats calls in hle_nvn.c */
```

Frontend services the backends call (sketch):

```c
/* nvn_state.h */
extern nvn_draw_state S;  extern nvn_rt_state R;            /* today's S and R, made extern: no getters on the hot path */
uint32_t nvn_state_apply(uint32_t op, const void *payload);  /* 0 = not a state op; else the dirty mask */
void     nvn_state_save(nvn_state_snap *out);                /* for look-ahead scans (pso_predict_rest) */
void     nvn_state_restore(const nvn_state_snap *in);
/* nvn_resolve.h */
uint64_t nvn_tex_slot_handle(int stage, unsigned slot);
uint64_t nvn_c2_handle(int stage, unsigned byte_offset);
uint32_t nvn_ubo_handle(int stage, int ubo_slot, int byte_offset);   /* includes inline updates queued earlier */
int      nvn_tid_from_handle(uint32_t h);  int nvn_sid_from_handle(uint32_t h);
void     nvn_fill_driver_bank(int stage, int bank, uint8_t *dst, size_t n, const nvn_refl *rf);
/* nvn_gpuwrite.h */
int  nvn_gw_hits(uint64_t lo, uint64_t hi);                  /* a GPU write queued or running overlaps */
void nvn_gw_note(uint64_t lo, uint64_t n, int kind);
/* nvn_pools.h */
int  nvn_pool_lookup(uint64_t addr, uint64_t len, nvn_pool_ref *out);   /* index, base, backend handle */
/* nvn_pipe.h */
int  nvn_pipe_from_state(nvn_pipe_desc *out, const nvn_shader *vs, const nvn_shader *fs, const uint16_t nvn_cfmt[8],
                         uint16_t nvn_dfmt, uint8_t samples);
int  nvn_pipe_parse_row(const char *line, nvn_pipe_desc *out);         /* R rows (Metal formats) and P rows */
```

### 1.6 What the graphics side needs from the CPU port and the platform layer

These cross the host boundary and must be agreed with the CPU-side design:

| Need | Today | On Windows and Linux |
|---|---|---|
| NVN entry thunks and the big host stack | generated AArch64 `nvn_thunks.S`, `nvn_entry.S`; Apple arm64 receives narrow integer arguments as 32 bits ([dm/gfx/README.md](../../dm/gfx/README.md), lines 12-14) [V] | x86-64 thunks from `gen_protos.py`, with the ABI bridge of the recompiler |
| Page-fault hook for dirty tracking | `dm_fault_hook_register`, `dm_guest_write_prep_register`, `dm_guest_write_prepare` (`dm/host/dm.h:131-135`, `dm/host/crash.c:102-114`); called from `hle_svc.c:454`, `469`, `587` and `hle_fs.c:731` [V] | the same three functions: a vectored exception handler and `VirtualProtect` on Windows, `SIGSEGV` and `mprotect` on Linux, or write watching (section 4.4) |
| Guest memory map and unmap events | `hle_svc.c` notifies the dirty tracker when a mapping opens a host page ([development.md](../development.md), lines 39-43) [V] | also `be_guest_map` / `be_guest_unmap`, so the Vulkan backend can release an import before the memory under it is decommitted (coalesced imports, section 4.2) |
| Time | Mach absolute time in `hle_nvn.c` and `nvn_dirty.c` [V] | a platform clock (`QueryPerformanceCounter`, `clock_gettime`) |
| Window surface | `front_metal_layer()` (`dm/host/front.h:15`) [V] | `front_vk_instance_extensions(const char **names, uint32_t *n)` and `front_vk_create_surface(VkInstance, VkSurfaceKHR *)`, implemented with SDL3 (`SDL_Vulkan_GetInstanceExtensions`, `SDL_Vulkan_CreateSurface`) |
| Progress screen | `front_progress` ("Preparing shaders") [V: `front.h`] | the same function, drawn by the SDL3 front end |
| Thread priorities | Mach QoS classes | below-normal or idle priority for background compiles (section 3.5) |

### 1.7 Migration order

Each step keeps the Metal backend's behaviour identical and is checked on a Mac with `make -C dm/gfx/tests GAME=<id> run` and `python3 scripts/regress.py check --games <the five games regress.py impact names>` ([06-dev-loop.md](../agents/06-dev-loop.md)), plus the new CPU tests on Linux and Windows. Steps M1 to M5 are moves of existing code with no change of logic, each in its own pull request.

| Step | Change | Extra check |
|---|---|---|
| M0 | Prerequisites with no graphics change: chain files checked out with LF line endings on every OS (section 13), `dmsh_chain.py` usable without `xcrun` for the Vulkan target, stale lines in the docs corrected | `python3 dm/gfx/tools/dmsh_chain.py` prints the committed `DMSH_CHAIN` (`157e1b96c8da`) on Windows and Linux; `dm/tools/tests/test_nsp_chain.py` passes there |
| M1 | `nvn_be.h` with `be_*` names; on Apple each is the existing `mtl_*` function (renamed or a static inline wrapper); the statistics calls of `hle_nvn.c` become `be_report`, which prints the same lines | the log of a 60 s boot of two games is identical line for line, timings excepted |
| M2 | move `S`, `R`, `state_op`, `bind_stages`, `prog_resolve`, `state_reset`, queue save and restore into `nvn_state.c`; handle helpers into `nvn_resolve.c`; the GPU-write ledger into `nvn_gpuwrite.c`; the pool index into `nvn_pools.c` | new CPU tests `t_state`, `t_gpuwrite` (also in the Linux CI) |
| M3 | the decode loop moves to `nvn_replay.c`; each `case` body of `mtl_exec` becomes the Metal `be_*` function verbatim; the look-ahead hooks at the top of the loop (`sdx_seq`, `hoist_take`, `lc_land`, deferred groups) become `be_rec_pre`; the look-ahead scanners stay in `nvn_mtl.m` | `DM_NVN_PASSDUMP_ALL` of one frame byte-identical before and after on two games; submit CPU time within run-to-run noise (`scripts/bench.py` with variants) |
| M4 | reflection parser in C, first producing exactly today's structs (Metal enum values included); then neutral texture type and pixel class enums, mapped to Metal's where the Metal backend uses them | a test parses every reflection JSON of five games' libraries with both parsers and compares the `nvn_refl` structs byte for byte; then `nvn_refl.m` goes |
| M5 | neutral pipeline description and manifest rows (`nvn_pipe.c`); inverse format table `nvn_fmt_from_mtl` in `dm/gfx/tex` | every `R` row of every profile's `pso_manifest.tsv` round-trips (row → neutral description → Metal key) to the same key, or is reported as not representable |
| M6 | build split: `dm/host/Makefile` source lists per host (Apple: `.m`, `.S`; Windows and Linux: `hostvk`, x86-64 thunks); the frontend compiles without Apple frameworks | a Linux and a Windows CI job build the frontend and run its CPU tests |
| M7 | Vulkan backend, new code only (section 11) | Vulkan tests under the validation layers |

Who runs the Mac checks of M1 to M5 is an open question (section 12): this machine is Windows, and `regress.py` needs a Mac with the games' NSP folders.

## 2. Shader chain for the Vulkan target

### 2.1 What the Vulkan path consumes

Today the chain is Maxwell code → SPIR-V (`dmsh_xlate`, the vendored Ryujinx 1.3.2 translator) → MSL (SPIRV-Cross with argument buffers, `dmsh_build.py:37`) → text rewrites of the MSL → metallib ([dm/gfx/README.md](../../dm/gfx/README.md), lines 29-46) [V].

The SPIR-V is already Vulkan SPIR-V: `Program.cs` creates the translator context with `TargetLanguage.Spirv, TargetApi.Vulkan` (`Program.cs:165`) [V]. Its accessor answers for Metal, though:

- every resource goes into set 0 with consecutive binding numbers, so SPIRV-Cross emits one argument buffer (`Program.cs:46-52`) [V];
- no depth clip control (`Program.cs:125`) and no depth rewrite: `dmsh_build.py` adds it in MSL [V];
- fixed-input compute jobs report no geometry passthrough (`Program.cs:114`) [V];
- FP64 is lowered to FP32 on a retry when the SPIR-V declares doubles (`Program.cs:130`, `175-180`) [V];
- the Metal image-atomics flag is always set (`Program.cs:164`) [V].

**Proposal.** The Vulkan target consumes the translator's SPIR-V directly, from a translator build whose accessor answers for Vulkan (2.3), followed by a few SPIR-V passes (2.2). SPIRV-Cross is not on the output path. The analyses that today read MSL text (integer vertex inputs, integer fragment outputs, the c1 index bounds of `c1_guard.py`, the uniform read extents of `cb_extent.py`) run unchanged on an MSL rendering of the Vulkan SPIR-V made only for analysis, so both targets take the same decisions from the same code.

### 2.2 Metal-specific steps and their Vulkan counterparts

From `finish` in `dm/gfx/tools/dmsh_build.py:888-961` and the files it calls [V]:

| Step today | Why it exists | On Vulkan |
|---|---|---|
| `vs_depth`: entry points `main0` (GL rewrite z' = z/2 + w/2) and `main0_z` (`dmsh_build.py:733`; [04-graphics.md](../agents/04-graphics.md), section 5) | Metal clips z < 0; the depth mode is run-time state | `VK_EXT_depth_clip_control` with `negativeOneToOne` per pipeline: the GL convention in fixed function, as Maxwell does it, so no variant and no rounding difference. Without the extension: one module with the rewrite behind a specialization constant |
| `vs_pad` (`dmsh_build.py:639`) | Metal refuses a pixel input the vertex shader does not write | the value would be undefined on Vulkan [A: interface matching rules; check with the validation layers]; Maxwell gives (0, 0, 0, 1). Kept, as a SPIR-V pass fed by the same pair tables |
| `vs_init` (`dmsh_build.py:656`) | unwritten components read (0, 0, 0, 1) on the hardware | kept: a SPIR-V pass stores (0, 0, 0, 1) into every generic output at entry |
| `int_inputs` and the re-translation (`dmsh_build.py:597`, `904-913`) | integer vertex inputs | same decision; the translator emits `uint` inputs from the job's `attr` facts |
| shadow inputs `<key>_bv` (`dmsh_vin.py`) | Metal reads a format only into an input of its class | Vulkan has the same rule [A: the numeric type of a vertex format and of the input must match; check with the validation layers]. Kept as SPIR-V shadow inputs at locations 16-30, which needs `maxVertexInputAttributes` of at least 31 [A: 32 on the desktop GPUs we target; checked at device selection, CPU conversion otherwise as today] |
| `fs_uint`: `main0_u`, `main0_m` (`dmsh_build.py:745`) | a float output cannot meet an integer attachment | same rule on Vulkan [A]: kept, as extra entry points of the same module, made by re-translation with `fout` facts |
| `vs_layer` (`dmsh_build.py:667`) | Metal cannot build a pipeline whose vertex function writes both the layer and a point size | not needed: the point size is ignored for non-point topologies [A] |
| `gs_slice`, `vs_layer_entry` (`dmsh_build.py:677`, `707`) | no geometry stage in Metal | optional: native geometry shaders exist; kept as an optimization where `shaderOutputLayer` (Vulkan 1.2 feature) is supported |
| `vs_tf` (`main0_tf`, `dmsh_build.py:393`) and `nvn_feedback.inc` | Metal has no transform feedback | native `VK_EXT_transform_feedback`: the layout from `tf_layout` (`dmsh_build.py:372`) goes to the translator, which emits `Xfb*` decorations |
| `vs_discard` (`main0_discard`, `dmsh_build.py:423`) | Metal needs a void vertex function with rasterization off | not needed: `rasterizerDiscardEnable` keeps vertex side effects |
| geometry as compute (`dmsh_geometry.py`, `nvn_geometry.inc`) | no geometry stage in Metal | native geometry shaders through the translator's ordinary path; passthrough shaders as ordinary ones (`QueryHostSupportsGeometryShaderPassthrough` answered false), `VK_NV_geometry_shader_passthrough` on NVIDIA later |
| tessellation, not drawn ([shaders.md](../reference/shaders.md), "Tessellation stages are not drawn") | Metal 4 render encoders have none | native tessellation control and evaluation shaders |
| `spv_single_lane` (`dmsh_build.py:776`) | Metal has no SIMD-group functions in vertex functions | not needed where `subgroupSupportedStages` includes the vertex stage; for Maxwell-exact results request subgroup size 32 (`VK_EXT_subgroup_size_control`, core in 1.3) where the device allows it; the rewrite stays as the fallback [A] |
| `precise` (`dmsh_build.py:41`) | `fast::min` / `max` / `clamp` leave NaN results to the GPU, Maxwell returns the other operand ([shaders.md](../reference/shaders.md), "min, max and clamp with a NaN operand") [V] | GLSL.std.450 `FMin` / `FMax` / `FClamp` are undefined with a NaN operand too [A]: rewritten to `NMin` / `NMax` / `NClamp` |
| `-fmetal-math-mode=relaxed` (`dm/ios/tools/ios_metal.py:8`) | the shaders reproduce Maxwell's NaN behaviour with `isnan` selects | the `SignedZeroInfNanPreserve` execution mode where `shaderSignedZeroInfNanPreserveFloat32` (float controls, core in 1.2) is supported, patched in when the module is created [A] |
| `-fpreserve-invariance` (`dmsh_build.py:50`) | identical position math must give identical depth across shaders ([shaders.md](../reference/shaders.md), "Position invariance across shaders") [V] | the `Invariant` decoration on the position, which Vulkan honours across pipelines |
| `-O1` (`dmsh_build.py:50`) | Apple's control-flow budget | not applicable |
| native texture atomics (`dmsh_atomic.py`, `MetalImageAtomics`) | Metal's atomics model; out-of-image coordinates must not write | native SPIR-V image atomics on R32 images; the bounds behaviour (IGN) stays, through the same translator path under a target-neutral name [A] |
| pulling entries `<key>_bp` (`dmsh_pull.py`) | Metal pipelines fix the vertex layout; stand-ins while a pipeline compiles | not needed: vertex input is dynamic state or a cheap pipeline library (section 3) |
| readable-tile variants `<key>_bf` (`dmsh_fetch.py`) | pass merging in tile memory | not needed |
| FP64 lowering (`Program.cs:175-180`) | Metal has no double | native FP64 where `shaderFloat64`; the lowered variant elsewhere (Tears of the Kingdom 1.4.3: 30 shaders, `Program.cs:175`) [V] |
| reflection parsed from the MSL argument-buffer struct (`reflect`, `dmsh_build.py:517-565`) | binding map | from `<key>.res.json` and the SPIR-V decorations |
| `c1_full.tsv`, `cb_extent.tsv` | c1 indices not proven bounded; uniform read extents | the same analyses, written as Vulkan-stamped tables; `robustBufferAccess2` (out-of-bounds uniform reads return 0) could replace the dedicated 64 KiB c1 banks [A: Maxwell reads 0 out of bounds of a constant bank] |
| `vertex_ab` (`dmsh_build.py:324`) | VertexA and VertexB parts | unchanged (Maxwell level) |

One Metal tolerance has no Vulkan counterpart: integer and stencil textures read through float declarations keep their bits on Apple GPUs ([shaders.md](../reference/shaders.md), "Integer textures and images through float declarations keep their bits"; [rendering.md](../reference/rendering.md), "Stencil-mode texture views read through `X32_Stencil8`") [V]. On Vulkan the sampled type of an image must match its format's numeric type [A: check with the validation layers]. A census of such reads in the built shaders, and a translator answer per slot (facts, or a second entry point), is a task of phase P2 and a risk (section 11).

### 2.3 Binding model

The Vulkan translator build assigns bindings by resource class and NVN slot instead of one running number:

| Set | Content, per stage (binding = stage base + slot) |
|---|---|
| 0 | uniform buffers: c0 (driver bank), c1 (the shader's constants), c2 (the handle table, which some shaders read as data: 243 in Engage, [04-graphics.md](../agents/04-graphics.md) section 3) [V], c3 to c16 (game uniform slots 0 to 13), the translator's support buffer |
| 1 | storage buffers, slots 0 to 15, and the geometry helpers |
| 2 | sampled textures and samplers: combined slots 0 to 127, separate textures 128 to 255 and separate samplers, numbered as the host numbers them (`NVN_SEPTEX_SLOT0`, `nvn_obj.h:157`) [V] |
| 3 | storage images and texel buffers |

- One pipeline layout per stage combination (vertex and fragment; with geometry; with tessellation; compute), the same for every shader, so any shader pair links with any other (pipeline libraries need compatible layouts) and descriptor sets can be shared and cached.
- The per-draw driver data (c0 with the draw's base instance at 0x644; c2 with the bound handles) is written into a frame ring and bound as one dynamic uniform buffer per stage, so a draw changes offsets, not descriptors. Five per pipeline stay within the smallest common limit of dynamic uniform buffers [A: `maxDescriptorSetUniformBuffersDynamic` is 8 on AMD; checked per device].
- Game buffers, textures and samplers: descriptor sets cached by a hash of their resolved content (consecutive draws mostly repeat them), from per-frame pools; push descriptors (core in 1.4; 92.15 % of Windows reports, [Sources](#sources)) [V] for small sets that change at every draw; `VK_EXT_descriptor_buffer` (70.09 % of Windows reports) [V] as an optional path after the descriptor microbenchmark of milestone P3.
- Handles read from the game's uniform buffers (`tex_ubo`, `samp_ubo`, [04-graphics.md](../agents/04-graphics.md) section 4) [V] keep being resolved on the CPU at replay by the shared resolver, which sees the inline updates queued earlier in the submission (`pend_read`). Fully dynamic handles (handle type 4: 121 texture reads in a census of Monster Hunter Rise's 23,478 shaders, [04-graphics.md](../agents/04-graphics.md) section 3) [V] become possible later with a descriptor-indexed array over the whole texture pool (descriptor indexing, core in 1.2).

### 2.4 Versioning and cache keys

- `DMSH_CHAIN` and its file list (`dmsh_chain.py:18-25`) stay as they are, so no Vulkan-only change touches the Metal libraries.
- A second version, `DMSH_CHAIN_VK`, in `dm/gfx/nvn/dmsh_chain_vk.h`, written by `dmsh_chain.py --target vk --header`. It hashes `dmsh_chain.py`, the build script and its Vulkan branch, the shared analysis modules, the sources of the Vulkan translator build (`dm/gfx/tools/dmsh_xlate_vk`), the same Ryujinx sources as the Metal list, `dmsh_facts.py`, the SPIR-V pass module, and `dmsh_chain_vk.lock` (fingerprints of `dmsh_xlate_vk.dll`, `Ryujinx.Graphics.Shader.dll`, `Ryujinx.Common.dll`, the embedded source hash, the analysis SPIRV-Cross per OS, glslang and spirv-val).
- A separate translator project (`dmsh_xlate_vk.csproj`, sharing the accessor's fact lookups with `dmsh_xlate` through one source file) keeps a Vulkan-only accessor change out of `dmsh_xlate.dll`, whose fingerprint is in the Metal lock (`dm/gfx/tools/dmsh_chain.lock`) [V]. A change to shared code (Ryujinx, the shared accessor file, the analyses) changes both versions; when it is meant for Vulkan only, `regress.py chain` must show byte-identical MSL on the Metal side.
- Libraries go to `<cache>/vk-<V>/`; each reflection JSON records `chain`; the Vulkan host loads only `vk-<DMSH_CHAIN_VK>/`.
- The profile inputs digest (`dmsh_chain.py --inputs`; the tables are listed at `dmsh_chain.py:26`) [V] is the same for both targets and stamps `vk-<V>/inputs.tsv`.
- The per-shader cache key (`shader_key`, `dmsh_build.py:105`) gains the target.
- AGENTS.md's procedure applies per target: after a Vulkan chain tool change, `--target vk --header`, committed with the change; `test_nsp_chain.py` checks both headers.

### 2.5 The tools on win-x64 and linux-x64

- **Translator.** `dmsh_xlate_bin` holds framework-dependent .NET 10 assemblies (`dmsh_xlate.runtimeconfig.json`: `net10.0`) and a macOS universal launcher (`dmsh_xlate`, a Mach-O) [V: `file`]. The assemblies run anywhere with `dotnet dmsh_xlate.dll`: on this Windows machine (.NET 10.0.12), `dotnet dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate.dll --source-sha` printed `f21af1efa737a270`, the lock's `xlate_source` [V: command]. The DLL fingerprints in the lock therefore hold on every OS, and no per-platform `dotnet publish` is needed. The scripts call `dotnet <dll>` where the launcher cannot run.
- **Lock check.** `tools_now` runs `xcrun -sdk … metal --version` (`dmsh_chain.py:81-83`) [V], which fails off a Mac; the Vulkan target's lock records no Metal compiler.
- **SPIRV-Cross.** The in-repo binary is a macOS universal Mach-O [V: `file`], built by `spirv_cross_bin/build.sh` from tag `vulkan-sdk-1.4.363.0` (`build.sh:12`) [V]. The analysis step on Windows and Linux uses the same tag built per OS, with a fingerprint per OS in the Vulkan lock.
- **Line endings.** This checkout has `core.autocrlf=true`, and the chain files are checked out with CRLF; `python3 dm/gfx/tools/dmsh_chain.py` prints `2aff3aff9a76` here instead of the committed `157e1b96c8da` [V: command]. Step M0 adds a `.gitattributes` rule that checks the chain files out with LF (their bytes on a Mac do not change, so no Metal rebuild). Without it, no shader build on Windows can match a Mac's.

### 2.6 Offline outputs per game

- `<cache>/vk-<V>/<key>.spv`: one module per shader with its entry points (`main`, `main_u`, `main_m`); `<key>_bv.spv` where shadow inputs are needed; `<key>_fp32.spv` for the few FP64 shaders; `<key>.json`: the neutral binding kinds of `nvn_refl.h`, set and binding, entry points, the fragment interpolation mask (section 3.2) and raster facts; `inputs.tsv`; and the profile's `c1_full_vk.tsv` and `cb_extent_vk.tsv` (not committed, like their Metal counterparts).
- The NSP script (`scripts/build_ipa_from_nsp.py`) gains `--platform windows` and `--platform linux`: a folder with the host, the game modules from the CPU-side tools, `vk-<V>/`, the profile tables and `pso_manifest.tsv`.
- A build by hand, sketched: `DM_GAME=<id> python3 dm/gfx/tools/dmsh_build.py --target vk <package> <cache>`.

### 2.7 The host's own shaders

The Metal backend compiles MSL at run time in 14 places (`newLibraryWithSource` at `nvn_mtl.m:3745`, `4893`, `7967`, `8058`, `8190`, `8268`, `8404`, `9287`, `9572`, `10063`, `10198`; `nvn_feedback.inc:42`; `nvn_geometry.inc:227`; `tex/nvn_deswizzle_gpu.m:22`) [V: grep]. The Vulkan backend has its own in GLSL under `dm/gfx/hostvk/shaders/`, compiled offline by glslang into SPIR-V committed as C arrays; a test rebuilds and compares them. Needed on Vulkan:

- the four counter kernels embedded in games' `main` (`nvn_builtin.h:15-25`, matched by control-section hash) [V];
- quad conversion (Vulkan has no quads), from `ixc_run` (`nvn_mtl.m:8187`);
- 8-bit index widening (`wide_run`, `nvn_mtl.m:8265`), only on devices without 8-bit indices;
- clear triangles for partial channel masks and partial stencil masks (`clear_color_tri`, `clear_ds_tri`);
- draw-based copies, only for what `vkCmdBlitImage` cannot do (format reinterpretation, depth and stencil scaling; stencil written through `VK_EXT_shader_stencil_export`);
- GPU deswizzle and ASTC decode (sections 5.2, 5.3), conditional-render predicates (4.7), the present pass (7).

Not needed on Vulkan: the transform feedback counter helpers, the geometry compaction kernels, the indirect-count masking (`mdx_run`, `nvn_mtl.m:8401`), and the readable-tile and magenta-scan diagnostics.

## 3. Pipelines without stutter

### 3.1 State model and hashing

**Today on Metal** [V]: a render pipeline key (`pso_key`, `nvn_pso_skey.h:6-10`) holds the two shaders, the attachment formats as `MTLPixelFormat` values, the sample count, blend enables, channel masks, alpha to coverage, the blend states, the vertex attributes and streams, and the merged-pass mapping; unused colour state is cleared (`pso_key_norm`, `nvn_pso_skey.h:22-27`). Depth-stencil state is a separate object; rasterizer state is encoder state. Metal 4 bases leave the colour state unspecialized, and a pipeline is specialized from its base in microseconds ([development.md](../development.md), "Pipeline bases and the pipeline archive").

**On Vulkan** the frontend fills an API-neutral description (`nvn_pipe_desc`, `nvn_pipe.c`) from `S` and the pass: the stage shader keys and entry points, the NVN attachment formats and sample count, blend, channel masks, alpha to coverage, logic operation, vertex attributes and streams, the topology class, rasterizer state (cull, winding with the queue's origin, polygon mode, depth clamp, rasterizer discard, depth bias enable, conservative rasterization, line mode), depth-stencil state, the alpha test, the patch size and the depth mode. The Vulkan backend hashes only the part the device cannot set dynamically:

| Dynamic with | State |
|---|---|
| Vulkan 1.0 | viewport, scissor, line width, depth bias values, blend constants, depth bounds values, stencil masks and reference |
| extended dynamic state (core in 1.3) | cull mode, front face, topology within its class, depth test, write and compare, depth bounds test, stencil test and operations, viewport and scissor counts, vertex binding strides |
| extended dynamic state 2 (core in 1.3, two optional features) | rasterizer discard, depth bias enable, primitive restart; logic operation and patch control points as optional features |
| extended dynamic state 3 (optional, feature by feature) | polygon mode, rasterization samples, sample mask, alpha to coverage, logic operation enable, colour blend enables, equations and write masks, depth clamp, depth clip negative-one-to-one, conservative rasterization, line rasterization mode, provoking vertex |
| `VK_EXT_vertex_input_dynamic_state` | vertex attributes and bindings |

With all of them a pipeline is its shaders, their entry points and the attachment formats: about as many pipelines as Metal has bases (Breath of the Wild's prebuild: 15,767 bases, [development.md](../development.md), "Hybrid pipelines") [V]. Without them, the missing parts join the key, as they are in Metal's key today.

### 3.2 Two phases with `VK_EXT_graphics_pipeline_library`

- **Libraries.** Four parts: vertex input (only without dynamic vertex input; cheap), pre-rasterization shaders (vertex, tessellation, geometry, with the universal layout and any non-dynamic rasterizer state), fragment shader (with non-dynamic depth-stencil state) and fragment output (attachment formats, non-dynamic blend state). The shader libraries depend on one shader each, not on a pair, so they can all be compiled at load time from the shader map and the prewarm lists.
- **First use.** A draw whose pipeline is missing links its libraries without link-time optimization: fast linking, typically well under a millisecond where `graphicsPipelineLibraryFastLinking` is reported [A: measure per device in P5]. The draw uses that pipeline at once.
- **Background.** The same libraries, created with `RETAIN_LINK_TIME_OPTIMIZATION_INFO`, are linked again with `LINK_TIME_OPTIMIZATION` on a background worker; the cache entry's pipeline handle is swapped when the optimized one is ready, and the fast one is destroyed when no submission in flight uses it.
- **Same output.** Both pipelines run the same SPIR-V with the same state, and the position carries `Invariant`, so depth matches across them as it must across a depth pre-pass and an equal-depth pass. Other outputs could still differ in the last bit if a driver optimizes floating-point code differently at link time [A]. `t_vk_gpl_identity` (section 10) renders every shader pair of a game's library both ways and compares the pixels bit for bit; a vendor where they differ gets the swap turned off in the backend's device quirk table, not in a game profile.
- **No stand-ins.** On Metal a draw may use the pair's pulling pipeline, a different vertex entry point proven to compute the same positions (`t_pull_inv`; [development.md](../development.md), "Hybrid pipelines") [V]. On Vulkan the fast-linked pipeline is the draw's own shaders, so nothing else is ever drawn.
- **Interpolation decorations.** Where `graphicsPipelineLibraryIndependentInterpolationDecoration` is false, a vertex library's `Flat` / `NoPerspective` outputs must match its fragment partner's inputs [A: the property's meaning, to check in the specification]; DXVK uses the extension only on drivers where the property holds ([Sources](#sources)) [V]. The reflection records each fragment shader's interpolation mask; the build gives each vertex shader its partners' decorations from the pair tables, and makes one vertex variant per distinct mask where partners disagree.

### 3.3 Prewarm lists, prediction and prebuild

- **Seed lists exist.** The profiles' `pso_manifest.tsv` files are not empty: Tears of the Kingdom has 4,092 lines (2,512 `R` render-pipeline rows), Pokémon Sword 3,273, Breath of the Wild 1,789 [V: counted]. ([development.md](../development.md), line 15 still says "The bundled prewarm lists are empty".) An `R` row is the hex of `pso_skey` (`nvn_pso_skey.h:17`; [development.md](../development.md), line 404) [V], whose formats are Metal pixel formats (`nvn_mtl.m:3279`, `3282`) [V]. The frontend decodes them through the inverse of `nvn_fmt_to_mtl` (`tex/nvn_fmt_mtl.c:76`) [V], skips merged-pass and readable-tile rows (Metal only), and hands the Vulkan backend a neutral description. `S` rows name shaders: the Vulkan backend compiles their libraries.
- **Per-device list.** The Vulkan backend appends what it builds on the spot to `pso_user_vk.tsv` beside the save, as portable `P` rows (NVN formats), and prewarms it after the seed at the next start.
- **Prepare step.** As on Metal ([development.md](../development.md), "The prepare step"): with a window the first submission waits until the lists are compiled, on every core, behind `front_progress` ("Preparing shaders"); the screen shows only past 0.5 s.
- **Prediction.** The look-ahead after a miss (`pso_predict_rest_`, `nvn_mtl.m:6853`; at most two speculative builds, [development.md](../development.md), line 414) [V] replays state only; with the state save and restore in the frontend (step M2), the Vulkan backend uses the same scan to queue fast links and library compiles ahead of the draws.
- **Prebuild.** Optionally (decision D3), the first start compiles the libraries of every shader in the map, the counterpart of the Metal prebuild of pulling bases from the pair tables (Breath of the Wild: 15,767 bases in 187 s with 7 workers on an M2 Max, [development.md](../development.md), "Hybrid pipelines") [V]. With the disk cache, later starts take seconds [A: measure on the RTX 4060 and an Intel integrated GPU].

### 3.4 Pipeline cache on disk

- One `VkPipelineCache` per game and device, created at start from `vk_pipeline_cache/<vendorID>-<deviceID>-<driverVersion>-<pipelineCacheUUID>-<DMSH_CHAIN_VK>.bin` beside the save. The driver validates the header (vendor, device, cache UUID) and ignores data that does not match; the file name covers the chain version and the driver version, so a new driver or chain starts a new file, and files of other versions are deleted at start.
- Written with `vkGetPipelineCacheData` to a temporary file and renamed, after the prepare step and 10 s after a burst of new pipelines (as the Metal archive flushes, `arch_note_new`, `nvn_mtl.m:5261`) [V], never while a compile could be under way on the same cache object (a merged copy of per-worker caches is written). The Metal archive's history of compaction crashes ([shaders.md](../reference/shaders.md), "A compaction flush that crashes, then crashes every start") [V] is why the write is copy-and-rename and the cache is never read back while being written.
- The drivers keep shader caches of their own as well [A]; ours does not depend on them.
- `VK_KHR_pipeline_binary` (53.27 % of Windows reports; NVIDIA since 553.00) [V] gives explicit binaries; optional, not needed for correctness.

### 3.5 Threads on low-core CPUs

A Core i3 has 4 cores (8 threads on recent ones, 4 on some) [A: check the user's target models], shared by the game's threads, the recompiled code, the submit thread and the drivers.

- **While the game waits** (prepare step, prebuild): one worker per hardware thread less one.
- **During play**: compiles the draws need (fast links, the libraries of a shader seen for the first time) at normal priority, demand first, as `pso_pend_demand` does today ([development.md](../development.md), "Hybrid pipelines") [V]; speculative builds limited to two, as today; link-time-optimized pipelines on one worker at idle priority (`THREAD_PRIORITY_IDLE` on Windows, `SCHED_IDLE` on Linux), started only while the frame step (`nvn_fstep.h`) reports slack, so they never take time from a frame that needs it.
- **Encoding**: Vulkan command buffers can be recorded on another thread, as `DM_NVN_ENC_THREAD` does for Metal (`nvn_rec.h:1-8`: about 40 % of the submit thread's time was inside Metal in Breath of the Wild) [V]. Off by default on four-thread CPUs until measured.

### 3.6 Required and optional extensions, and the fallbacks

Coverage below is the share of Windows device reports on gpuinfo.org ([Sources](#sources)) [V]: `VK_EXT_graphics_pipeline_library` 71.96 %, `VK_EXT_extended_dynamic_state` 89.16 %, `_2` 84.67 %, `_3` 73.08 %, `VK_EXT_vertex_input_dynamic_state` 79.63 %, `VK_EXT_shader_object` 57.76 %. On Linux the graphics pipeline library is at 90.25 % [V].

| Level | Device | Pipeline key | A missing pipeline costs | Stutter control |
|---|---|---|---|---|
| A | pipeline libraries, dynamic states 1-3, dynamic vertex input (Intel's Arc / Arrow Lake driver lists all three [V]; NVIDIA's has had the libraries since 2022 [V]; other drivers per section 9 [A]) | shaders, entry points, formats | a fast link; a library compile only for a shader never seen before | libraries compiled at load; the fast link at first use |
| B | pipeline libraries, partial dynamic state 3 | plus the blend and raster states the device cannot set | a fast link, plus a fragment-output or pre-rasterization library when its state is new | as A; the state libraries are small |
| C | no pipeline libraries, dynamic states and vertex input | shaders, formats, the non-dynamic state | a full compile | prewarm, prediction, optional prebuild, disk cache; the draw waits for its own pipeline |
| D | none of them (expected on Intel's legacy driver for 6th to 10th generation processors, see section 9) [A] | everything Metal's key holds today | a full compile | as C, with more permutations: the prebuild of D3 matters most here |

In every level a draw is made with its own shaders and its own state: when no pipeline is ready it waits (a stutter, counted and logged as `DM_PSO_WAITLOG` does today, [dm/gfx/README.md](../../dm/gfx/README.md), line 162) [V], and it is never skipped or drawn with something else. Shader objects (`VK_EXT_shader_object`, or Khronos's emulation layer, which uses pipeline libraries where it can, [Sources](#sources)) are an option for later on drivers that implement them natively; Intel's Windows driver 32.0.101.8974 does not list the extension [V].

## 4. Memory and synchronization

### 4.1 The guest memory model

- An NVN buffer address is the CPU address of pool storage the game allocated itself (`nvnMemoryPoolGetBufferAddress`, `hle_nvn.c:220`) [V]. Pools have flags (`CPU_NO_ACCESS`, `CPU_UNCACHED`, `CPU_CACHED`, `GPU_*`, `COMPRESSIBLE`, `PHYSICAL`, `VIRTUAL`: `nvn.h:606-617`) [V], which the Metal backend never receives (`mtl_pool_add(o->mem, o->size)`, `hle_nvn.c:203`) [V], because on unified memory every pool is a zero-copy buffer.
- Textures are separate GPU objects (block-linear storage deswizzled into private textures), uploaded from their storage when the CPU wrote it and mirrored back into it when the GPU copies into such a texture ([nvn-objects.md](../reference/nvn-objects.md), section "CPU-written textures") [V].
- What any backend must keep: (1) every guest address a game passes (vertex, index, uniform, storage, indirect, counter, event) is GPU-addressable; (2) a CPU write is seen by a later GPU read without a flush, because games write uncached pools and some never flush (Tears of the Kingdom, FINAL FANTASY TACTICS, The Great Ace Attorney Chronicles: [nvn-objects.md](../reference/nvn-objects.md), "CPU writes with no flush, caught by page protection") [V]; (3) a GPU write is seen by the CPU once its fence has completed.

### 4.2 Pool backing on Vulkan

- **Import guest memory (every GPU).** `VK_EXT_external_memory_host` (88.04 % of Windows reports; listed by Intel's Arc / Arrow Lake driver) [V] imports host pages as `VkDeviceMemory`, with one `VkBuffer` over each import. The pool lookup then gives (buffer, offset), as `mtl_gpu_addr` (`nvn_mtl.m:556`) gives (`MTLBuffer`, offset) today. The GPU reads and writes guest memory itself, as on Apple's unified memory: exact semantics (1)-(3) with no copy, and on an integrated GPU, whose memory is system memory, it is also the fastest way.
- **How many imports.** One import per pool, rounded to `minImportedHostPointerAlignment` [A: 4 KiB on the desktop drivers we target], mirrors today's one buffer per pool. Breath of the Wild reached 4,006 live pools on an iPhone ([nvn-objects.md](../reference/nvn-objects.md), "Memory-pool tracking table grows in blocks") [V], close to the 4,096-allocation limit some Windows drivers report, which the backend's own allocations share [A: `maxMemoryAllocationCount`; check per driver]. Where the limit is low, pools inside one committed guest region share an import of that region's span, rebuilt (not grown) when a pool falls outside it, as sparse pools' runs are rebuilt today (`vpool_*` in `hle_nvn.c`, [nvn-objects.md](../reference/nvn-objects.md), "Sparse virtual pools built from separate physical pools") [V]; an old import is released after the last submission using it completes. Whether two imports may cover the same page (adjacent pools sharing one) must be verified per driver [A]; on Metal overlapping no-copy buffers are verified by `dm/gfx/metal/probe/p_nocopy.m` ([nvn_metal_mapping.md](../../dm/gfx/nvn_metal_mapping.md), line 17) [V].
- **Device-local mirrors (discrete GPUs).** Over PCIe the GPU's reads of system memory are slower than of its own memory [A: measure on the RTX 4060 laptop, PCIe 4.0]. Granules (64 KiB) of pools that the GPU reads often and the CPU has not written for a while are copied into device-local buffers, and bindings that fall in them use the copy. The copy is protected by the same dirty-page generations as packaged textures (`nvn_dirty_since_span`, `nvn_dirty.c:197`) [V]: a CPU write after the copy re-uploads the granule in order before the next submission that reads it. Ranges any GPU operation writes (storage buffers the reflection marks written, transform feedback, copy and fill destinations, query results; the GPU-write ledger knows them) are never mirrored and stay in guest memory. Off on integrated GPUs.
- **Resizable BAR.** A device-local, host-visible heap larger than 256 MiB [A: the usual sign of resizable BAR] lets the CPU write mirror granules straight into video memory instead of through a staging copy (decision D7 covers the memory it takes).
- **Placed maps.** `VK_EXT_map_memory_placed` (0.93 % of Windows reports) [V] could put device memory at the guest address on Linux; not planned.
- **Images** are device-local, as Metal's private textures. Linear textures in CPU-visible pools (Mario's glyph cache: [nvn-objects.md](../reference/nvn-objects.md), "Linear textures in CPU-visible pools") [V] are sampled from a linear image bound to the imported memory where the format allows linear sampling, else from a copy refreshed by the dirty generations [A: `linearTilingFeatures` per format].

### 4.3 Uploads and readbacks

- **Packaged and CPU-written textures** are deswizzled on the GPU by a compute pass that reads the imported pool buffer and writes the image: no CPU deswizzle and no staging copy. The re-upload decisions are today's (`pkg_upload_`, `nvn_mtl.m:2097`; dirty generations). The CPU deswizzle (`tex/nvn_deswizzle.c`) stays as the fallback; `VK_EXT_host_image_copy` (listed by Intel's Arc / Arrow Lake driver) [V] can write images from the CPU directly on integrated GPUs.
- **Buffer → texture copies** (`OP_COPY_B2T`) read the imported pool buffer with `vkCmdCopyBufferToImage`; **texture → buffer** copies write it with `vkCmdCopyImageToBuffer`, and the CPU sees the result after the fence [A: imported memory is host-coherent on the drivers we target; else `vkInvalidateMappedMemoryRanges`].
- **ReadTexels** reads guest memory when the content is there, as `mtl_read_texels` does (`nvn_mtl.m:9109`) [V], else copies the image into a staging buffer and waits.
- **GPU copies into textures uploaded from their storage** (`nvn_tex_mirror_t2t`, `mirror_readback`: [nvn-objects.md](../reference/nvn-objects.md), "GPU copies into textures uploaded from their storage") [V] keep their mechanism: a GPU copy into a readback buffer, swizzled into the storage by the completion handler.

### 4.4 Dirty-page tracking

- Today: `nvn_dirty.c` write-protects the storage of uploaded textures (`nvn_dirty_protect`, `nvn_dirty.c:179`), the fault handler (`on_fault`, `nvn_dirty.c:91`) unprotects and stamps a generation, and host code that writes guest memory calls `dm_guest_write_prepare` first (`hle_fs.c:731`, `hle_svc.c:454`, `469`, `587`) [V].
- **Windows**: the same scheme through the platform layer (`VirtualProtect`, a vectored exception handler), or write watching: guest memory reserved with `MEM_WRITE_WATCH` (the host reserves it, so it can) and `GetWriteWatch` with reset at each submission over the ranges the GPU uses, with no faults at all [A: cost over gigabytes of ranges; whether kernel writes are reported].
- **Linux**: `mprotect` and `SIGSEGV` as today, or `PAGEMAP_SCAN` with userfaultfd's asynchronous write protection, which reports written pages with no faults [A: Linux 6.7 or later].
- x86-64 pages are 4 KiB against 16 KiB on Apple silicon, NVN's own granularity, so fewer textures share a page with unrelated data ([nvn-objects.md](../reference/nvn-objects.md), "Writes beside a packaged texture in a shared host page") [V].
- Opportunity outside this design: fault-free write watching could track every texture in a CPU-visible pool, turning the per-game keys `tex_flush_upload` and `tex_dirty_upload` (9 and 7 profiles) [V: grep] into a general rule, on both backends.

### 4.5 Queues, fences, syncs and events

| NVN | Metal backend today [V] | Vulkan backend |
|---|---|---|
| queues; submissions serialized under one lock (`hle_nvn.c:1179-1184`) | one Metal 4 queue | one graphics and compute `VkQueue`; a second queue of that family for the presenter when `queueCount` > 1 |
| `SubmitCommands` | command buffer segments (`sub_cut`, `nvn_mtl.m:9520`; 64 draws by default) | the same segments, each a `vkQueueSubmit2` that signals the timeline semaphore at the segment's fence value |
| fence values | an `MTLSharedEvent` | a timeline semaphore (core in 1.2) |
| `QueueFenceSync`, `OP_FENCE_SYNC` (`hle_nvn.c:657`, `nvn_mtl.m:9744`) | last / segment fence value | the same values |
| `SyncWait` (`hle_nvn.c:665-672`) | waits for the event, then makes the CPU-side writes up to the fence (`post_early`) | `vkWaitSemaphores`, then the same post ledger (in the frontend after M2) |
| `QueueWaitSync`, `CommandBufferWaitSync` | nothing: one queue in order (`hle_nvn.c:659-660`) | nothing |
| `QueueFinish` | waits for the last fence, not for the display ([nvn-objects.md](../reference/nvn-objects.md), "`nvnQueueFinish` waits for the GPU, not the display") | `vkWaitSemaphores` on the last value |
| `SignalEvent` | a CPU write after completion (`mtl_sub_event`, `nvn_mtl.m:1205`) | the same |
| `ReportCounter` | CPU write after completion, samples from visibility buffers (`nvn_mtl.m:9732-9737`) | GPU writes in order (4.6) |

A completion thread waits on the timeline for the next pending value and runs the post ledger, in place of Metal's commit feedback handler.

### 4.6 Queries and counters

- An NVN report is 16 bytes, a 64-bit value and a 64-bit timestamp (`nvn_builtin.h:7`) [V]. Today only samples passed come from the GPU (visibility buffers); every other report is written by the CPU after the submission completes, with the segment's GPU times (`rep_ticks`, `nvn_mtl.m:1227`), so a GPU consumer in the same submission cannot read it ([04-graphics.md](../agents/04-graphics.md), section 7, `DM_NVN_UB_PARTIAL`) [V].
- Vulkan: precise occlusion queries for samples passed; `vkCmdWriteTimestamp2` for `TIMESTAMP` and `TIMESTAMP_TOP`, converted to NVN's 614.4 MHz units (`nvn_timestamp.h`) by a small compute pass, with `VK_KHR_calibrated_timestamps` (listed by Intel's Arc / Arrow Lake driver) [V] tying GPU ticks to the guest clock; pipeline statistics, `VK_EXT_primitives_generated_query` and transform-feedback queries for the other types [A: map each NVN counter type in P8]. `vkCmdCopyQueryPoolResults` writes the results to the report address in the imported pool buffer. Copies cannot run inside a render pass, so a report inside a pass is copied at the pass end, before anything outside the pass can read it.
- Results: each timestamp report carries its own GPU time (Breath of the Wild's frame interval controller and dynamic resolution, NieR's raw timestamps: [frame-rate.md](../reference/frame-rate.md), "GPU timestamps per submission segment" and "Raw NVN timestamp units") [V], so the profile key `gpu_timestamps` is not needed on Vulkan; the counter kernels of `nvn_builtin.h` read real values in the same submission.

### 4.7 Conditional rendering

- NVN draws when the 64-bit counters at `addr` and `addr + 16` are equal or not equal (`p_render_cond`, `nvn_obj.h:112`) [V]. The host records the condition only for profiles with `render_cond` (`hle_nvn.c:2382-2389`, four profiles) [V: grep] and evaluates it on the CPU at replay (`nvn_mtl.m:9738-9743`, `rc_value` at 594); a condition on an occlusion query keeps drawing, and Super Mario Odyssey's same-frame query condition is still ignored because CPU evaluation from earlier frames made it worse ([shaders.md](../reference/shaders.md), "Position invariance across shaders", note) [V]. ([development.md](../development.md), line 15, still says "Conditional render is always drawn".)
- Vulkan: `VK_EXT_conditional_rendering` (87.48 % of Windows reports; listed by Intel's Arc / Arrow Lake driver) [V]. At the condition a one-thread compute pass writes `a == b` or `a != b` into a predicate word of a per-submission buffer, from the counters in guest memory, which the GPU itself wrote in order (4.6); draws and clears up to the next `SetRenderEnable` run inside `vkCmdBeginConditionalRenderingEXT` on that word. A clear under a condition is a `vkCmdClearAttachments` inside the conditional block, never a load action. The compute pass cannot run inside a render pass: a condition met inside a pass ends it, unless the GPU-write ledger shows nothing writes those counters since the pass began, in which case the predicate is computed before the pass.
- This is NVN's behaviour for every game, so `render_cond` is not needed on Vulkan; the CPU evaluation remains the fallback on a device without the extension. Checked on the four `render_cond` games and on Super Mario Odyssey (section 10).

### 4.8 Indirect and indirect-count draws

- NVN's indirect records (`{count, instances, first, base instance}` and `{count, instances, first index, base vertex, base instance}`, `nvn_obj.h:160-161`) [V] have the field order of `VkDrawIndirectCommand` and `VkDrawIndexedIndirectCommand`, so the GPU reads them from the pool buffer as they are.
- `MultiDrawArraysIndirectCount` / `MultiDrawElementsIndirectCount` (`hle_nvn.c:2283-2290`) become `vkCmdDrawIndirectCount` / `vkCmdDrawIndexedIndirectCount` with the game's count address and stride (`drawIndirectCount`, core in 1.2; listed by Intel's Arc / Arrow Lake driver) [V]. The record copies of `mdx_run` (`nvn_mtl.m:8401`) go away. ([nvn_metal_mapping.md](../../dm/gfx/nvn_metal_mapping.md), line 53, still lists these two entry points as empty.)
- Triangle fans are native; 8-bit indices are native with `VK_KHR_index_type_uint8` (core in 1.4; 88.04 % EXT / 70.84 % KHR of Windows reports) [V]; quads still need the index conversion (2.7).

### 4.9 Ordering inside a submission

- Vulkan orders nothing between commands without barriers, as Metal 4 orders nothing inside one encoder ([rendering.md](../reference/rendering.md), "Metal 4 does not order operations inside one encoder") [V]. The GPU-write ledger that decides the Metal copy encoder's barriers decides Vulkan's between transfers, dispatches and draws.
- Between render passes the first version uses a full barrier, as Metal's default `DM_NVN_BARRIER=full` ([04-graphics.md](../agents/04-graphics.md), section 7) [V]; the game's barrier bits refine it after measurement.
- Inline uniform updates and copies cannot run inside a render pass on Vulkan either [A: Vulkan's rules for `vkCmdUpdateBuffer` and `vkCmdCopyBuffer`]. The uniform deferral and shadows of the Metal backend (`mtl_sub_update`, `nvn_mtl.m:1161`; `ubp_get`, `nvn_mtl.m:1121`; the extents of `cb_extent.tsv`) are therefore needed by the Vulkan backend too: they move to the frontend when the Vulkan backend reaches them (milestone P4), with the Metal backend calling the same code.
- A fragment-ordering barrier after draws that wrote images ends the pass on Metal (`swrite_note`, [rendering.md](../reference/rendering.md), "Fragment-ordering barriers between draws that write images") [V]; on Vulkan likewise, or a barrier inside the pass where `VK_KHR_dynamic_rendering_local_read` (core in 1.4) covers it [A: which accesses a local-read barrier orders].

## 5. Textures

### 5.1 Formats

A table `dm/gfx/tex/nvn_fmt_vk.c` beside `nvn_fmt_mtl.c`, in the same pure-C form (NVN format → `VkFormat`, component mapping, exact or substitute), with the usages checked at run time through `vkGetPhysicalDeviceFormatProperties2`. Where it differs from the Metal table (`tex/nvn_fmt_mtl.c:28-71`) [V]:

- **BC1 to BC7 and BC6H** are native on desktop GPUs [A: `textureCompressionBC` on every device we target].
- **Three-component formats** (0x19-0x24) have Vulkan formats, but sampling them with optimal tiling is rarely supported [A]: expanded to four components at upload as on Metal, unless the device reports the usage.
- **16-bit packed formats** exist in Vulkan in both bit orders (`R5G6B5`, `B5G6R5`, `R5G5B5A1`, `A1R5G5B5`, `A1B5G5R5` with maintenance5, `R4G4B4A4`, `B4G4R4A4`, and `A4R4G4B4` / `A4B4G4R4` with `VK_EXT_4444_formats`, which Intel's Arc / Arrow Lake driver lists) [V for the extension], so the substitutes marked unverified on Metal (`nvn_fmt_mtl.c:49-51`, [dm/gfx/tex/README.md](../../dm/gfx/tex/README.md), line 48) [V] can be exact once the bit order is checked with real data.
- **Integer-read-as-float vertex formats** (`*_UI2F`, `*_I2F`, 0x51-0x68) are Vulkan's `USCALED` / `SSCALED` vertex formats [A: vertex-buffer support on the devices we target], so the CPU conversion Metal needs for them (`vconv_i2f`; [rendering.md](../reference/rendering.md), "Vertex formats Metal lacks") [V] is not.
- **Signed RGB10A2** (0x69, 0x6A) maps to `A2B10G10R10_SNORM_PACK32` / `SINT` where supported [A], else unpacked as on Metal.
- **Depth 24 with stencil** (0x35) maps to `D24_UNORM_S8_UINT` where supported (NVIDIA, Intel) [A]; AMD's driver does not expose it [A: check format properties]. Metal stores it as 32-bit float (`nvn_fmt_mtl.c:43-47`) [V], which changes what one unit of constant depth bias means (fixed 2^-24 for a 24-bit normalized format, exponent-dependent for float) [A: Vulkan's depth bias rules]. Where the device lacks the format, `D32_SFLOAT_S8_UINT` with `VK_EXT_depth_bias_control` asking for a normalized-format bias [A: availability per vendor]; decision D10 covers a device with neither.
- **Format views** (an sRGB view of a linear texture, integer views of images) need `MUTABLE_FORMAT` with a format list (core in 1.2), so the driver can keep compression [A].
- **ASTC**: 5.2.

### 5.2 ASTC

- Nintendo's games use ASTC widely: 4×4 (STEINS;GATE RE:BOOT's images), 5×5 (Donkey Kong Country: Tropical Freeze's logo and menus), 6×6 (Monster Hunter Rise's streamed textures), 8×8 (Tears of the Kingdom's terrain arrays, two 121-layer arrays of 42 MB each) ([rendering.md](../reference/rendering.md) and [nvn-objects.md](../reference/nvn-objects.md)) [V].
- Hardware ASTC: Apple GPUs. On desktops, Intel graphics up to generation 12.0; generation 12.5 and later (Xe-HP, Arc Alchemist) have none, and Mesa decodes on the GPU for them [V: Phoronix]. NVIDIA and AMD desktop GPUs have none [A: `textureCompressionASTC_LDR` false; check with `vulkaninfo`].
- Where the device has it, ASTC stays compressed: nothing changes. Elsewhere a compute pass decodes each ASTC texture at upload, from the imported pool memory (4.3), into one of:

| Option | Fidelity | Memory per texel (ASTC 4×4 / 6×6 / 8×8 take 8 / 3.56 / 2 bits) | Load time |
|---|---|---|---|
| RGBA8 (sRGB formats into an sRGB format) | identical to a decoder in 8-bit mode (`VK_EXT_astc_decode_mode`); within half a unit of the 8th bit of the default 16-bit decode for linear formats [A: ASTC decode precision rules] | 32 bits: 4× / 9× / 16× the ASTC size | GPU decode, about memory speed [A] |
| RGBA16F for linear formats, RGBA8 sRGB for sRGB formats | identical to the default decode [A] | 64 bits for linear formats: 8× / 18× / 32× | as above |
| BC7, encoded at upload on the GPU | a second lossy generation; a real-time encoder is weaker than an offline one [A] | 8 bits: 1× / 2.25× / 4× | an encode pass per texture |
| BC7, encoded offline or cached on disk after a first encode | a second lossy generation, better encoder | as above | first start (or the NSP script) pays the encode |

  Tears of the Kingdom's two terrain arrays alone would take 672 MB each as RGBA8 [V: 16 × 42 MB]. The choice is the user's (decision D1). Video memory is watched with `VK_EXT_memory_budget` (listed by Intel's driver) [V].
- The decoder is validated bit for bit against a reference CPU decoder (ARM's `astcenc`, Apache-2.0 [A: licence]) on synthetic blocks of every mode and block size, and on game textures dumped locally (never committed). The game files stay in their NSP folders, as AGENTS.md requires.

### 5.3 Layouts and deswizzling

`nvn/nvn_tex.c` (layouts, from deko3d) and `tex/nvn_deswizzle.c` are API-neutral and shared as they are [V]. The GPU deswizzle kernel of `tex/nvn_deswizzle_gpu.m` gets a GLSL counterpart that reads the imported pool buffer; both are checked against the cases of `t_tex` and `t_deswz_gpu`. Sparse pools keep their 4 KiB page table and `nvn_sparse_copy` ([nvn-objects.md](../reference/nvn-objects.md), "Textures read through a 4 KB page table of a sparse pool") [V]; Vulkan sparse residency is not used.

### 5.4 Samplers

- **Border colours.** Metal has three presets and the host picks the nearest (`nvn_mtl.m:4185`) [V]. `VK_EXT_custom_border_color` (86.73 % of Windows reports; Intel's driver lists it) [V] gives the exact colour, float or integer (`nvnSamplerBuilderSetBorderColori` / `ui`, `hle_nvn.c:561-562`) [V]; the nearest preset remains the fallback.
- **Reduction.** Metal ignores min / max reduction with a nearest or no mip filter, so the host filters such samplers' mips linearly ([rendering.md](../reference/rendering.md), "Sampler reduction (min / max)") [V]; Vulkan's reduction mode (core in 1.2) takes the sampler as the game made it [A: reduction applies with every mip filter].
- **LOD bias.** The Metal backend never applies NVN's LOD bias: `lod_bias` appears in `nvn_mtl.m` only in the pass dump (`nvn_mtl.m:6354`) [V: grep]. Vulkan's `mipLodBias` applies it.
- Anisotropy and depth compare are native; LOD snap has no counterpart here either (`hle_nvn.c:564`) [V].

### 5.5 Render targets, views, feedback, multisampling, depth

- **Usage flags** are chosen at creation from the NVN texture flags (render target, `IMAGE`, compressible), so the Metal backend's upgrades (a texture copied into a new one with more usage: [rendering.md](../reference/rendering.md), "Render-target use added on demand", "Integer views for images a shader declares uint or int") [V] are not needed; their cost in lost compression is measured per vendor [A].
- **Compressed textures written through uncompressed views** (agl's BC4 encoder) can use block-texel-view-compatible images (core in 1.1) [A: whether such a view can be a colour attachment]; otherwise the Metal backend's shadow copy through a buffer ([rendering.md](../reference/rendering.md), "Compressed textures as render targets through an uncompressed shadow") [V]. A depth texture drawn through a colour view needs the shadow on Vulkan too.
- **Duplicate attachments** (one texture at two colour slots) keep the frontend rule of `rt_dup_slot` (`nvn_mtl.m:3083`) [V].
- **Clears.** Whole-image clears outside a pass are `vkCmdClearColorImage` / `vkCmdClearDepthStencilImage`, with exact integer values; a scissored clear of all channels inside or outside a pass is `vkCmdClearAttachments` with the scissor rectangle, which closes the Metal case of a depth-stencil clear with a scissor outside a pass that is cleared whole ([rendering.md](../reference/rendering.md), "Clears inside a pass, with a partial mask or a partial scissor") [V]; partial channel masks and partial stencil masks stay clear triangles. A clear of an attachment larger than the pass ([rendering.md](../reference/rendering.md), "Clears of attachments larger than the render pass") [V] is an image clear before the pass. The Depth16 clear value keeps `nvn_depth16_clear_value` (`nvn_depth_clear.h`) [V].
- **Same-pass feedback.** Four profiles list fragment shaders in `feedback_snap` (Super Mario Bros. Wonder, Super Mario 3D World, NieR:Automata, Captain Toad) [V: grep], because a snapshot for every draw that samples its own attachment costs too much on Apple GPUs (Breath of the Wild 16.6 → 36.6 ms a frame) and the A20 Pro returns broken blocks without one ([rendering.md](../reference/rendering.md), "Same-pass feedback read through a snapshot") [V]. On Vulkan every such draw is detected (the sampled subresource is attached in the current pass) and drawn with `VK_EXT_attachment_feedback_loop_layout` (73.08 % of Windows reports; Intel's driver lists it) [V] and the matching pipeline flag; a game barrier that invalidates textures between such draws becomes a by-region barrier inside the pass [A]. Without the extension: the general layout and a snapshot for every feedback draw, its cost measured on desktop GPUs. No per-game list.
- **Multisampling.** Native multisample images; `Downsample` of a multisample source is `vkCmdResolveImage`, of a single-sample one a filtered blit ([rendering.md](../reference/rendering.md), "`Downsample` of a single-sample texture") [V]. 8 and 16 samples where the device has them [A]. NVN's programmable sample locations, ignored today (`hle_nvn.c:1104`) [V], use `VK_EXT_sample_locations` (Intel's driver lists it) [V].
- **Depth formats**: D16, D32F and D32F_S8 native; D24_S8 as in 5.1; S8 as `S8_UINT` where supported [A]. A stencil-mode view is a stencil-aspect view, read with an integer type (section 2.2).

### 5.6 Profile keys that exist for Apple GPU or Metal behaviour

| Key (profiles using it) [V: grep] | Why it exists | On Vulkan |
|---|---|---|
| `feedback_snap` (4) | undefined same-pass reads on Apple GPUs | not needed (5.5) |
| `pso_mtl4` (1) | Metal 4 pipelines on iOS | not applicable |
| `present_snap` (1) | frees the game's swapchain texture before the display link takes it ([frame-rate.md](../reference/frame-rate.md), "Present snapshots: the game's swapchain texture free at once") | not needed: the present copies the game's texture into a swapchain image, which frees it when the copy completes (section 7) |
| `depth_mode` (12) | picks `main0` or `main0_z` for offline Metal pipeline bases | not needed with depth clip control: the depth convention is pipeline state |
| `gpu_timestamps` (2) | report times per submission segment | not needed (4.6) |
| `render_cond` (4) | the CPU evaluation is not always right | not needed where `VK_EXT_conditional_rendering` exists (4.7) |
| `vconv_range` (1) | the CPU vertex conversion over bound ranges | probably not needed: more vertex formats are native (5.1) [A] |
| `tex_dyn_slots` (3) | translator fallback arrays read as slot arrays | still needed (translation semantics) |
| `tex_flush_upload` (9), `tex_dirty_upload` (7) | CPU writes to texture storage | still needed (memory model); a candidate for a general rule (4.4) |

## 6. Features Metal lacks and Vulkan has

| NVN feature | Metal backend today [V] | Vulkan | Requirement |
|---|---|---|---|
| geometry shaders | compute emulation (`nvn_geometry.inc`) for the shapes it covers; others do not build (Big Brain Academy's line renderer, Monster Hunter Rise 53, Luigi's Mansion 3 10: [shaders.md](../reference/shaders.md), "Geometry shaders with no Metal stage") | native | `geometryShader` [A: all desktop GPUs we target] |
| tessellation | not drawn (Luigi's Mansion 3, Bayonetta 3: [shaders.md](../reference/shaders.md), "Tessellation stages are not drawn"); `SetPatchSize` is a no-op (`hle_nvn.c:1208`) | native | `tessellationShader` [A] |
| transform feedback | a capture entry point; indexed and indirect capture open ([rendering.md](../reference/rendering.md), "Transform feedback captured by a vertex entry point") | native | `VK_EXT_transform_feedback` (92.9 % of Windows reports) [V] |
| conditional rendering | CPU evaluation behind a profile key | GPU evaluation (4.7) | `VK_EXT_conditional_rendering` (87.48 %) [V] |
| indirect-count draws | record copies by compute (`mdx_run`) | native | `drawIndirectCount` (core in 1.2) |
| logic operations | recorded (`hle_nvn.c:2118`) but never applied: `logic_op` appears in `nvn_mtl.m` only in a pass-dump line (`nvn_mtl.m:8895`) | native | `logicOp` [A] |
| alpha test | ignored: the translator is told `AlphaTestOp.Always` (`Program.cs:121`) | a compare and discard at the end of the fragment shader, the function as a specialization constant and the reference in a push constant | none; first a census of games that set it |
| advanced blend modes | Porter-Duff modes as fixed-function factors; multiply, screen, overlay and the HSL modes keep the state's functions with a red line ([rendering.md](../reference/rendering.md), "Advanced blend modes as Porter-Duff factors") | native on NVIDIA; elsewhere as on Metal | `VK_EXT_blend_operation_advanced` (47.29 %) [V] |
| wide lines | 1-pixel lines only (`hle_nvn.c:2401`) | native | `wideLines`, line rasterization (92.9 %) [V] |
| depth bounds | no-op (`hle_nvn.c:1209`) | native | `depthBounds` [A] |
| conservative rasterization | ignored, open on Pokémon Scarlet ([rendering.md](../reference/rendering.md), "Conservative rasterization remains unimplemented") | native | `VK_EXT_conservative_rasterization` (Intel's driver lists it) [V] |
| programmable sample locations | ignored (`hle_nvn.c:1104`) | native | `VK_EXT_sample_locations` (Intel's driver lists it) [V] |
| viewport swizzles | (x, −y) as a flipped viewport; others a red line ([rendering.md](../reference/rendering.md), "Viewport swizzles applied as a flipped viewport") | native on NVIDIA; elsewhere a vertex-shader swizzle by specialization constant | `VK_NV_viewport_swizzle` [A] |
| exact border colours | three presets | native | `VK_EXT_custom_border_color` (86.73 %) [V] |
| sampler LOD bias | ignored | native | core |
| fully dynamic texture handles | unsupported (Monster Hunter Rise: 121 texture reads) | a descriptor array over the texture pool | descriptor indexing (core in 1.2) |
| 8-bit indices, triangle fans | widened or converted on the CPU or GPU | native | `VK_KHR_index_type_uint8` (core in 1.4); fans in core |
| subgroup operations in vertex shaders | rewritten for one lane | native | `subgroupSupportedStages` [A] |
| double precision | lowered to single | native where supported | `shaderFloat64` [A] |
| provoking vertex | not configurable | either convention | `VK_EXT_provoking_vertex` (84.67 %) [V]; which one NVN programs is to be checked [A] |

Quads have no Vulkan primitive either and keep the conversion.

## 7. Presentation and frame pacing

### 7.1 What the Metal backend does today [V]

- A present thread draws the game's swapchain texture onto a `CAMetalLayer` drawable (`present_now`, `nvn_mtl.m:10414`): the crop of `nvnWindowSetCrop`, the stored bytes as the Switch scans them out (an sRGB texture sampled through a non-sRGB view, `present_raw_src`), the flip of the lower-left origin, letterboxing, bilinear scaling ([nvn_metal_mapping.md](../../dm/gfx/nvn_metal_mapping.md), line 29; [rendering.md](../reference/rendering.md), "Lower-left window origin").
- Pacing comes from a `CAMetalDisplayLink` (`link_main`, `nvn_mtl.m:10645`; `link_range`, `nvn_mtl.m:10633`): 60 Hz ticks, virtual 60 Hz ticks on 144 and 165 Hz screens, frames of interval 2 held for their period, the acquire waiting as many ticks as the game's present interval, a rescue when the link goes silent, optional present snapshots ([frame-rate.md](../reference/frame-rate.md), "Present interval and display rate").
- `DM_PRESENT_UPSCALE=metalfx` runs MetalFX's spatial scaler in the present; it is off by default on every platform, and rendering at the screen's resolution stays the reference for sharpness ([rendering.md](../reference/rendering.md), "The present upscales through MetalFX"; `fx_encode`, `nvn_mtl.m:10300`).
- The frame step (`nvn_fstep.h`) judges lateness by what the screen showed.

### 7.2 The Vulkan presenter

- A swapchain on the SDL3 window's surface (`front_vk_create_surface`), format `B8G8R8A8_UNORM` (or `R8G8B8A8_UNORM`) in `SRGB_NONLINEAR` colour space: the present pass copies the game texture's bytes unchanged (an sRGB texture through a UNORM view, as `present_raw_src`), so the screen gets what the Switch scans out. HDR stays off: the Switch outputs SDR.
- The present pass is a full-screen triangle sampling the game's texture with crop, letterbox, origin flip and bilinear scaling, into the acquired image, submitted on the presenter's queue after a wait for the timeline value of the frame's last submission.
- The game's texture is free when that copy completes, so the acquire can hand it back then: what `present_snap` buys on Apple, by construction.
- Three images with FIFO by default (`minImageCount` respected). `VK_EXT_swapchain_maintenance1` / `VK_KHR_swapchain_maintenance1` (the EXT on 49.53 % of Windows reports; Intel's driver lists neither) [V] gives present fences and present-mode switches where available. Borderless fullscreen by default; `VK_EXT_full_screen_exclusive` (Windows; Intel's driver lists it) [V] optional.

### 7.3 The vsync source and pacing

`hle_nvn.c`'s pacing (the acquire waits for the present interval's ticks, the display-rate modes, the frame step) stays; the backend supplies the ticks and the display times through `be_vsync_*`:

1. `VK_KHR_present_id` with `VK_KHR_present_wait`: a thread waits for each present to reach the screen and takes the ticks and the on-screen intervals from it. NVIDIA has it since driver 471.69 [V]; AMD's Windows notes list present-wait extensions in 25.11.1 [A: from a search summary of AMD's page, which timed out here]; Intel's Windows driver 32.0.101.8974 does not list it [V]; 42.62 % / 43.74 % of Windows reports [V].
2. Windows without it: a thread blocked in DXGI's `IDXGIOutput::WaitForVBlank` for the window's output [A: a platform-layer function].
3. Linux without it: the compositor's presentation feedback through the platform layer, or the steady clock the windowless host uses today [A].

`VK_EXT_present_timing` (NVIDIA prerelease since November 2025) [V] can replace these later. On a fixed-rate screen that is not a multiple of 60 Hz the virtual-tick logic of `link_range` runs on these ticks. With a variable-refresh screen in fullscreen and FIFO, frames are shown when they arrive [A: compositor and driver behaviour]. MAILBOX is an option for lower latency (decision D12); IMMEDIATE (tearing) is not offered. On-screen intervals are checked with PresentMon on Windows besides the host's own `DM_NVN_TSV` rows [A: tool availability].

### 7.4 Resolution and upscaling

- Games render at their own resolution; the render-scale setting (0.5 to 1.0) lowers it for builds with a render-scale stub (`docs/env_switches.tsv`, `DM_RENDER_SCALE`) [V]. The present scales the picture into the window.
- MetalFX's counterpart on Vulkan would be a spatial upscaler run as compute passes in the present: AMD FidelityFX Super Resolution 1 (edge-adaptive upsampling and sharpening) or NVIDIA Image Scaling, both distributed as shader source under the MIT licence [A: licences], both running on any vendor's GPU. Neither gives MetalFX's pixels, and any spatial upscaler changes the picture compared with plain scaling: a quality question for the user (decision D4). Off by default, as on Apple.
- Integer scaling (nearest neighbour at whole multiples) as an option for pixel-art games.
- Rendering above the game's own resolution (scaling the game's render targets) is not part of this design: it changes what the game draws and needs per-game care.

## 8. Games that render with Vulkan or OpenGL

The [games table](../../dm/games/README.md) lists 84 NVN games, 3 Vulkan games (Hades, Super Mario Galaxy, Super Mario Galaxy 2) and 2 OpenGL games (Digimon Story Cyber Sleuth: Complete Edition, Balatro) [V: count of the table's rows].

### 8.1 Guest Vulkan: a pass-through shim

Today `dm/gfx/vk/hle_vk.m` implements Vulkan on Metal: one physical device shaped like the Tegra X1 (Vulkan 1.1, one queue family, three memory types on one heap), command lists replayed into Metal at `vkQueueSubmit`, shaders translated offline by `vk_shaders.py` and found by the hash of their SPIR-V, swapchain images presented through the NVN present path (`hle_vk.m:1-14`) [V]. On a Vulkan host most of that disappears. A new `dm/gfx/hostvk/guest_vk.c` registers the same entry points in the host's import table and forwards them to the host driver:

- **Device.** The guest's instance and device wrap the host device the presenter uses (`be_native_device`). Properties and limits are shaped like today's Tegra X1 device where the game reads them, so it takes the code paths it takes on the Switch; the extension list is what the game asks for that the host has.
- **Memory.** The guest sees the Switch's unified memory types; memory it maps is host-visible (in resizable BAR when present); images are device-local whatever memory type the game binds, as `hle_vk.m` already ignores image memory bindings [V].
- **Shaders.** The game's SPIR-V goes to the driver: no translation and no offline step. Each module passes `spirv-val` at first sight, with fix-ups for what NVIDIA's driver accepts and others may not: Super Mario Galaxy's `ArrayStride` on struct members ([rendering.md](../reference/rendering.md), "Vulkan shaders generated at run time") [V]; its second blend source written at output location 1 without an `Index` decoration, which NVIDIA's hardware feeds to source 1 ([rendering.md](../reference/rendering.md), "Dual-source blending from output location 1") [V] and other drivers may not [A].
- **Pipelines** are the game's own (Hades creates 1,856 before its first frame: [development.md](../development.md), line 422) [V]; the disk cache (3.4) makes later starts fast.
- **Queries** are native. **Present**: the guest's swapchain images are host images handed to the presenter with the layer crop of `vi::SetLayerCrop` ([rendering.md](../reference/rendering.md), "Swapchain pre-transform and layer crop") [V]; the acquire and the vsync event follow the vsync source ([frame-rate.md](../reference/frame-rate.md), "Vulkan acquire and vsync event follow the display link") [V].
- **Reused**: the import-table registration (`VK` / `VKB` macros), `DM_VK_CAPTURE`, the layer crop, the pacing hooks. The Metal translation stays Apple-only.

### 8.2 Guest OpenGL

Today `dm/gfx/gl/hle_gl.m` implements NVIDIA's desktop OpenGL 4.5 core profile through EGL on Metal: its own state tracking, GLSL translated offline (glslang to SPIR-V, then SPIRV-Cross) and found by the hash of its source, presents through the NVN present path (`hle_gl.m:1-19`; [rendering.md](../reference/rendering.md), "GLSL source translated offline and found by hash") [V]. Three ways on Windows and Linux (decision D5):

1. **Re-target `hle_gl.m`'s state machine to the Vulkan backend.** `gl_shaders.py` already produces SPIR-V with glslang (`-V -R`) [V], which the Vulkan path takes directly; one presenter; offline and deterministic like the NVN path. The work is a Vulkan encoder for the GL layer.
2. **The host's OpenGL driver**, through an SDL3 GL context: calls passed through, GLSL compiled by the driver at run time, frames handed to the presenter through GL-Vulkan interop (`GL_EXT_memory_object`, `GL_EXT_semaphore`) [A: support per driver]. Digimon's shaders are GLSL ES 3.20 ([rendering.md](../reference/rendering.md), "GLSL source translated offline and found by hash") [V], which a desktop driver accepts only with ES 3.2 compatibility [A].
3. **Zink** (Mesa's OpenGL on Vulkan), bundled: a large dependency whose Windows support is not mature [A].

Recommendation: 1, after the NVN path; 2 as a stopgap if these games are wanted earlier.

## 9. Minimum requirements

### 9.1 Proposed baseline (decision D6)

- **Required**: Vulkan 1.3 (dynamic rendering, synchronization2, timeline semaphores, extended dynamic state 1 and 2, subgroup size control, texel buffer alignment); `VK_KHR_swapchain`; `VK_EXT_external_memory_host` (a copy-based fallback is possible later, section 4.2); the features `geometryShader`, `tessellationShader`, `logicOp`, `depthClamp`, `depthBiasClamp`, `fillModeNonSolid`, `wideLines`, `largePoints`, `samplerAnisotropy`, `textureCompressionBC`, `multiDrawIndirect`, `drawIndirectFirstInstance`, `drawIndirectCount`, `independentBlend`, `dualSrcBlend`, `imageCubeArray`, `sampleRateShading`, `shaderClipDistance`, `shaderCullDistance`, `occlusionQueryPrecise`, `pipelineStatisticsQuery`, `vertexPipelineStoresAndAtomics`, `fragmentStoresAndAtomics`, `shaderStorageImageExtendedFormats` [A: the exact list follows from P3; every item is standard on the desktop GPUs below].
- **Used when present, with the fallback named in its section**: Vulkan 1.4 (push descriptors, 8-bit indices, line rasterization, dynamic rendering local read, maintenance 5 and 6: [Khronos](#sources)) [V: from a search summary of Khronos's announcement], `VK_EXT_graphics_pipeline_library`, `VK_EXT_extended_dynamic_state3`, `VK_EXT_vertex_input_dynamic_state`, `VK_EXT_descriptor_buffer`, `VK_KHR_present_id` and `VK_KHR_present_wait`, `VK_EXT_conditional_rendering`, `VK_EXT_transform_feedback` (without it the games that capture feedback lose those effects until a fallback exists), `VK_EXT_depth_clip_control`, `VK_EXT_attachment_feedback_loop_layout`, `VK_EXT_custom_border_color`, `VK_EXT_provoking_vertex`, `VK_EXT_conservative_rasterization`, `VK_EXT_sample_locations`, `VK_EXT_blend_operation_advanced`, `VK_EXT_depth_bias_control`, `VK_EXT_memory_budget`, `VK_KHR_calibrated_timestamps`, `VK_EXT_host_image_copy`, `VK_KHR_pipeline_binary`, `VK_EXT_shader_object`, ASTC.

### 9.2 Per vendor

| GPU and OS | Driver, as verified | Vulkan | Notes |
|---|---|---|---|
| NVIDIA GeForce (Turing to Blackwell), Windows and Linux | developer beta 597.33 (Windows) and 595.44.16 (Linux), 22 October 2026 [V: NVIDIA] | 1.4 [V] | graphics pipeline library since 473.33 (2022), shader object since 531.54, pipeline binary since 553.00, present wait since 471.69, descriptor heap since January 2026, present timing prerelease since November 2025 [V: NVIDIA]. The RTX 4060 laptop (Ada) is in this group |
| NVIDIA Maxwell, Pascal, Volta | the 580 branch is their last [V: Phoronix] | 1.4 [A] | the features above that predate 580 [A: check `vulkaninfo` on a GTX 10 card] |
| AMD Radeon, Windows | `VK_EXT_graphics_pipeline_library` listed under 25.5.1 (7 May 2025) and present-wait extensions under 25.11.1 on AMD's Vulkan release-notes page [A: a search engine's summary; the page timed out here] | 1.4 [A] | no `D24_UNORM_S8_UINT` [A]; which GPU generations current drivers cover [A] |
| AMD Radeon, Linux (RADV) | Mesa main: Vulkan 1.3 and 1.4 complete for `radv` [V: Mesamatrix]; DXVK 3 requires RADV 25.0 or later [V: DXVK] | 1.4 | |
| Intel Arc and Core Ultra (Arrow Lake) graphics, Windows | 32.0.101.8974: Vulkan 1.4.356, 159 and 158 device extensions; lists the graphics pipeline library, extended dynamic state 3, dynamic vertex input, descriptor buffer, push descriptor, external memory host, conditional rendering, transform feedback, 8-bit indices, depth clip control, attachment feedback loop, custom border colour, provoking vertex, conservative rasterization, sample locations, texel buffer alignment, host image copy, memory budget, calibrated timestamps; does not list present id / wait, shader object, pipeline binary, swapchain maintenance1, advanced blend [V: Geeks3D] | 1.4 [V] | no ASTC hardware from generation 12.5 on [V: Phoronix] |
| Intel Core 11th to 14th generation graphics (Xe-LP: Iris Xe, UHD 7xx), Windows | the current branch [A] | 1.3 or 1.4 [A] | ASTC in hardware [A: removal starts at generation 12.5]; missing swapchain extensions on Iris Xe reported [V: Intel community] |
| Intel Core 6th to 10th generation graphics (UHD 6xx) and Ice Lake, Windows | legacy support: 31.0.101.2141 (6 April 2026), Vulkan 1.3, mostly security fixes [V: Intel release notes] | 1.3 [V] | extension list not verified here [A]; expected at level D of section 3.6 [A]; ASTC in hardware [A] |
| Intel, Linux (ANV) | Vulkan 1.3 and 1.4 complete for `anv` [V: Mesamatrix]; graphics pipeline library since Mesa 23.2, later on by default [V: Phoronix]; ASTC emulated on generation 12.5+ since Mesa 23.3 [V: Phoronix]; Ivy Bridge and Haswell (`hasvk`) 91.3 % of 1.3 [V: Mesamatrix], not supported | 1.4 | |
| Mesa lavapipe (software) | Vulkan 1.3 and 1.4 complete [V: Mesamatrix] | 1.4 | continuous integration only |

Share of Windows reports on gpuinfo.org with each extension [V]: graphics pipeline library 71.96 %, extended dynamic state 1 / 2 / 3 89.16 / 84.67 / 73.08 %, dynamic vertex input 79.63 %, descriptor buffer 70.09 %, push descriptor 92.15 %, external memory host 88.04 %, present wait / id 42.62 / 43.74 %, conditional rendering 87.48 %, transform feedback 92.9 %, 8-bit indices EXT / KHR 88.04 / 70.84 %, depth clip control 80.75 %, attachment feedback loop 73.08 %, custom border colour 86.73 %, shader object 57.76 %, provoking vertex 84.67 %, advanced blend 47.29 %, swapchain maintenance1 (EXT) 49.53 %, placed memory maps 0.93 %, dynamic rendering local read 70.65 %, line rasterization 92.9 %, list restart 82.43 %, pipeline binary 53.27 %. These are shares of submitted reports, not of players' machines.

Windows 10 support by the vendors' current drivers in 2026 is to be checked per vendor [A].

## 10. Testing

1. **Frontend CPU tests**, built and run on macOS, Linux and Windows in CI: new `t_state`, `t_gpuwrite`, `t_replay_order` (the order of `be_*` calls for recorded command lists, against a recording backend), `t_pipe_rows` (every `R` row of every profile decodes, section 1.7 M5), `t_refl_c` (the C reflection parser against the Objective-C one), beside the existing pure-C tests (`t_hmap`, `t_cover`, `t_pace`, `t_timestamp`, `t_fstep`, `t_tex_layout`, `t_sparse_layout`, `t_dirty_*`, `t_link_ticks`, `dm/gfx/tex/tests/t_tex`).
2. **Vulkan GPU tests** in `dm/gfx/tests/vk/`: ports of the Metal tests that check NVN semantics rather than Metal mechanics, among them `t_queue_origin` (389 checks of culling, origins and swizzles: [rendering.md](../reference/rendering.md), "Queue initialization captures the raster origin") [V], `t_clear_ds`, `t_clear_tex`, `t_depth16_clear`, `t_feedback`, `t_cube_placeholder`, `t_view_swizzle`, `t_stencil_view`, `t_integer_blend`, `t_primitive_restart`, `t_rast_discard`, `t_transform_feedback`, `t_geometry`, `t_vbuf`, `t_float_pad`, `t_readback`, `t_linear_readback`, `t_copy_alias`, `t_perf_order` (its ordering cases); and new ones: `t_vk_gpl_identity` (3.2), `t_vk_cond_render`, `t_vk_queries`, `t_vk_astc` (bit-exact against the reference decoder), `t_vk_import` (host-memory import limits: allocation count, alignment, overlap, as `p_nocopy.m` probes Metal), `t_vk_pipeline_cache` (write, reload, invalidation). Not ported: tests of Metal mechanisms (`t_pso_archive`, `t_pso_bundle`, `t_present_rescue`, `t_present_alloc`, `t_fuse_check`, `t_defer_chain`, `t_reorder_chain`, `t_zskip`, `t_xv`, `t_rt_range*`, `t_pull*`, `t_upgrade_alias`).
3. **Linux in Docker with lavapipe.** An image pinned to a Mesa release whose lavapipe implements Vulkan 1.4 (recorded in the image tag), with the Khronos validation layer (synchronization validation on) and a debug messenger that fails a test on any error. A GitHub Actions job runs items 1 and 2 there with no GPU.
4. **Real GPUs.** The RTX 4060 laptop runs items 1 and 2 and the games; its integrated GPU, if it is an Intel or AMD one, through a device-selection switch (`DM_VK_DEVICE=<index>`) [A: which integrated GPU the laptop has]. An Intel 6th to 10th generation machine is needed for level D (section 3.6) [A: availability].
5. **Image comparison against Metal.** Reference captures are taken on a Mac with `DM_NVN_SHOT` at fixed frames of the `smoke.tsv` routes that `regress.py` uses (same saves, same scripted input, `DM_DETCLOCK` where the game allows it) and compared with the Vulkan captures by NVIDIA FLIP (`dm/third_party/flip`, BSD 3-Clause, built with `make -C dm/third_party/flip`) [V]. The third-party README says FLIP serves image measurements in `dm/verify/gfxq/`, which does not exist [V], so a new tool, `dm/tools/gfx_compare.py`, reports FLIP's mean and maximum per frame; `DM_NVN_TEXDUMP` dumps localize a difference to a pass. The threshold is calibrated from two Metal runs of the same route (the noise floor) and from Vulkan on NVIDIA against Metal; floating-point differences between GPU vendors are expected, structural differences are bugs.
6. **The regression harness** gains a Vulkan mode for Windows and Linux machines that have the games (smoke routes, chain checks of `vk-<V>`), so `regress.py check` covers both backends.
7. **Validation in normal runs**: `DM_VK_VALIDATION=1` turns the validation layer on (GPU-assisted validation for out-of-bounds accesses on demand).

## 11. Phased plan, milestones and risks

### 11.1 Phases

| Phase | Content | Acceptance check |
|---|---|---|
| P0 | M0 (section 1.7): line endings, chain tooling off the Mac, the stale doc lines | `dmsh_chain.py` prints the committed version on Windows, Linux and the Mac; `test_nsp_chain.py` passes on Windows |
| P1 | M1 to M6: the frontend, move by move | per step: `regress.py check` passes on five games on a Mac, the Mac GPU tests pass, the new CPU tests pass on three OSes; M3: a byte-identical full pass dump on two games |
| P2 | Vulkan chain: `dmsh_xlate_vk`, SPIR-V passes, `vk-<V>` outputs, the census of integer and stencil texture reads | for three games (Super Mario Bros. Wonder, Breath of the Wild, Metroid Dread): every shader that builds for Metal builds for Vulkan, except documented features; `spirv-val` clean |
| P3 | Vulkan backend core on lavapipe, headless: device, imported memory, textures (CPU deswizzle first), shaders, monolithic pipelines, draws, copies, clears, queries, sync; descriptor microbenchmark | item 2 of section 10 passes under validation; the first frames of a 2D NVN game headless; `DM_NVN_SHOT` within the FLIP threshold of the Metal reference; the microbenchmark fixes the binding scheme |
| P4 | Windows on the RTX 4060: SDL3 window, presenter, vsync source; uniform deferral and shadows in the frontend | five games reach gameplay; on a steady route at 60 fps the on-screen interval p99 stays within one vsync; no validation error |
| P5 | Pipelines: libraries, fast link, background optimization, disk cache, prewarm from `R` rows, prediction, prebuild option | level A, cold start with the seed lists: no draw waits more than 2 ms on the scripted route; warm start: no compile; levels C and D forced on the same machine: identical pictures (FLIP 0 against level A) with the waits counted; `t_vk_gpl_identity` bit-exact or a recorded quirk |
| P6 | Discrete-GPU memory: mirrors, resizable BAR | Breath of the Wild's route: GPU time with and without mirrors; identical pictures |
| P7 | Textures: ASTC (D1), packed formats, D24S8 (D10) | `t_vk_astc`; Monster Hunter Rise and Tears of the Kingdom ASTC scenes within the FLIP threshold; memory within the budget of an 8 GB GPU [A] |
| P8 | Native features (section 6), feedback loops, conditional rendering | the open items in the game notes render: Big Brain Academy's lines, Luigi's Mansion 3's tessellated title, Pokémon Legends: Z-A's character preview and Donkey Kong's fur (transform feedback), NieR's flight geometry, Super Mario Odyssey's same-frame conditions, each compared with Metal where Metal draws it |
| P9 | Guest Vulkan shim; guest OpenGL (D5) | the 3 Vulkan and 2 OpenGL games reach the scenes their notes describe |
| P10 | Linux packaging (SDL3 on Wayland and X11), Intel legacy (level D), thread budget on four-thread CPUs, reference docs | measured on the oldest supported machine; findings filed in `docs/reference` as AGENTS.md says |

### 11.2 Risks, ranked

1. **The extraction changes the Metal backend's behaviour.** A 10,989-line file with many file-scope globals and order-dependent look-ahead. *Retire*: move-only pull requests, a byte-identical pass dump in M3, `regress.py` on five games at every step, the Metal look-ahead left where it is.
2. **Stutter on devices without pipeline libraries** (about 28 % of Windows reports; Intel's legacy driver expected). *Retire*: measure levels C and D early (P5) by turning the extensions off on the RTX 4060 and on real legacy hardware; prewarm from the existing seeds; the prebuild option (D3).
3. **Integer and stencil textures read through float declarations**, which Apple tolerates and Vulkan does not (2.2). *Retire*: the census in P2, a translator answer per slot, the validation layers.
4. **CPU cost per draw on a Core i3.** Breath of the Wild has about 4,900 draws a frame ([development.md](../development.md), line 505) and its submit thread took about 6.5 ms a frame on Apple hardware, 2.7 ms of it inside Metal (`nvn_rec.h:2`; the source does not name the device) [V]. *Retire*: the descriptor microbenchmark in P3 at that draw count; a budget of at most the Metal backend's submit time; the encoding thread as an option.
5. **Discrete GPUs reading imported host memory too slowly.** *Retire*: measure in P4, mirrors in P6.
6. **Import limits** (allocation count, alignment, overlapping imports). *Retire*: `t_vk_import` in P3 on every vendor.
7. **Floating-point differences between vendors** (NaN handling, invariance, link-time optimization). *Retire*: the SPIR-V passes of 2.2, float controls, `t_vk_gpl_identity`, the FLIP thresholds.
8. **Pacing without present wait** (Intel on Windows, older AMD drivers). *Retire*: the fallback vsync sources of 7.3, measured with PresentMon.
9. **ASTC memory** on 4 to 8 GB GPUs and on integrated GPUs that share system memory. *Retire*: decision D1, the memory budget extension, measurement in P7.
10. **Driver bugs on AMD and Intel under Windows.** *Retire*: validation in CI, a per-vendor quirk table in the backend (never a game profile key), real-GPU runs before each release.
11. **Dependency on the CPU port** (thunks, platform layer, SDL3). *Retire*: the interfaces of 1.6 agreed first; the Vulkan backend developed against a Linux build of the host.
12. **Reproducible chains across OSes.** *Retire*: P0.

## 12. Decisions that need the user

Each item gives the options and the criteria; none is decided here.

| # | Decision | Options | Criteria |
|---|---|---|---|
| D1 | ASTC on GPUs without ASTC (all NVIDIA and AMD desktop GPUs, Intel from generation 12.5) | RGBA8 decode; RGBA16F decode for linear formats; BC7 encoded at upload; BC7 encoded once and cached on disk (table in 5.2) | fidelity (identical to an 8-bit or a 16-bit decode, or a second lossy generation); video memory (4× to 32× the ASTC size against 1× to 4×); first-start and load time |
| D2 | Memory allocator of the Vulkan backend | AMD's Vulkan Memory Allocator (a C++ single-header library, MIT [A]) in one C++ file of the host; or an allocator of our own in C | brings C++ into the host build (the host is C and Objective-C today); maturity (budgets, defragmentation, dedicated allocations); code to maintain |
| D3 | First start on devices without pipeline libraries (levels C and D) | prewarm lists only; or a prebuild of every shader's pipelines known from the pair tables before the first frame | first-start time (minutes on a slow CPU [A]) against stutter the first time a scene is seen; disk use |
| D4 | Spatial upscaler at present | none (bilinear, as today's default); FSR 1; NVIDIA Image Scaling | sharpness against fidelity to the game's own picture; GPU cost on an integrated GPU; licences |
| D5 | Guest OpenGL (2 games) | re-target the GL layer to Vulkan; the host's OpenGL driver; Zink | effort, fidelity, one presenter, dependencies |
| D6 | Hardware floor | Vulkan 1.3 with the list of 9.1; or Vulkan 1.4; and whether Intel 6th to 10th generation graphics (legacy driver, level D) and NVIDIA Maxwell to Volta (580 branch) are supported | reach against stutter and test effort |
| D7 | Video memory for buffer mirrors and resizable BAR on discrete GPUs | on by default with a budget fraction; opt-in | GPU time against video memory left for textures (D1) |
| D8 | Pipeline cache on disk | size limit and cleanup policy | disk use against compile time at start |
| D9 | New build tools and libraries | glslang (host shaders), SPIRV-Tools (`spirv-val`, maybe `spirv-opt`), volk (MIT, C) or our own Vulkan loader, SDL3 (already agreed) | licences, size, CI images |
| D10 | Depth 24 + stencil where the device has neither the format nor depth-bias control | 32-bit float depth with the game's bias unchanged; a bias conversion [A: exactness]; refuse the device | shadow-acne or peter-panning differences against reach |
| D11 | Double precision | native where supported plus a lowered variant elsewhere; lowered everywhere as on Metal | exactness for about 30 shaders of one game against one more variant |
| D12 | Latency against smoothness | three images with FIFO (default); a low-latency mode (two images, or MAILBOX) | input latency against dropped or uneven frames |
| D13 | Who runs the Mac regressions of steps M1 to M5 | the user on a Mac with the games; another contributor; a Mac in CI with game files kept outside the repository | M1 to M5 cannot be checked on this Windows machine |

## 13. What in the repository makes the plan harder

1. **Line endings change the chain version.** On a Windows checkout with `core.autocrlf=true` the chain files have CRLF line endings and `dmsh_chain.py` prints `2aff3aff9a76` instead of `157e1b96c8da` [V: command]. No Windows shader build can match a Mac's until M0.
2. **The chain tools assume a Mac**: `tools_now` runs `xcrun` (`dmsh_chain.py:81-83`), the translator launcher and SPIRV-Cross are Mach-O binaries [V]. The translator's assemblies do run on Windows [V: command].
3. **Prewarm rows hold Metal pixel formats** (`nvn_mtl.m:3279`, `3282` via `nvn_pso_skey.h:6-10`) [V], so the existing seed lists must be decoded through an inverse format table (M5).
4. **Reflection carries Metal types** (`dtt` is a `MTLTextureType`, `aid` an argument-buffer index; parsed with `NSDictionary`: `nvn_refl.h:28-29`, `58-62`) [V].
5. **Pool flags never reach the replay** (`hle_nvn.c:203`) [V]: the discrete-GPU memory design needs them.
6. **One 10,989-line file** holds the decoder, the NVN state and the Metal encoding, with Apple threading primitives throughout (`dispatch_*` 91, `os_unfair_lock` 102, Foundation `NS*` 848) [V: grep], and the graphics tests compile it directly and link Metal (`dm/gfx/tests/Makefile:18`) [V], so none of today's GPU tests runs off Apple until the frontend exists.
7. **Platform code inside the NVN layer**: the AArch64 thunks (`nvn_thunks.S`, `nvn_entry.S`) and Mach time in `hle_nvn.c` and `nvn_dirty.c` [V].
8. **MSL compiled at run time in 14 places** (section 2.7) [V]: what Vulkan still needs of them gets GLSL counterparts.
9. **A Metal tolerance the chain relies on**: integer and stencil textures read through float declarations (2.2) [V on Apple; A for Vulkan].
10. **Stale documentation**: [development.md](../development.md) line 15 still says conditional rendering is always drawn, indirect-count draws wait on the CPU, geometry shaders do not translate and the bundled prewarm lists are empty, all contradicted by [rendering.md](../reference/rendering.md) ("Conditional rendering evaluated at replay", "Indirect-count multi-draws"), [geometry-metal-research.md](../geometry-metal-research.md) and the manifests themselves; [nvn_metal_mapping.md](../../dm/gfx/nvn_metal_mapping.md) line 53 lists `MultiDraw*IndirectCount` as empty implementations, while `hle_nvn.c:2283-2290` implements them [V].
11. **No image-comparison harness**: FLIP is vendored, but the `dm/verify/gfxq/` its README names does not exist [V].

## Sources

External sources, read on 2026-10-28. "Fetched" means the page itself was read; "search summary" means only a search engine's summary of it was available, which section 9 marks [A] where it matters.

- gpuinfo.org, Vulkan extension coverage, Windows: <https://vulkan.gpuinfo.org/listextensions.php?platform=windows> (fetched).
- gpuinfo.org, `VK_EXT_graphics_pipeline_library` coverage per platform: <https://vulkan.gpuinfo.org/displayextensiondetail.php?extension=VK_EXT_graphics_pipeline_library> (fetched).
- NVIDIA, Vulkan driver support (developer beta release notes): <https://developer.nvidia.com/vulkan-driver> (fetched).
- Phoronix, "NVIDIA Confirms 580 Linux Driver Is The Last For Maxwell / Pascal / Volta": <https://www.phoronix.com/news/NVIDIA-580-Linux-Driver-Last-HW> (search summary).
- AMD, Radeon Vulkan release notes: <https://www.amd.com/en/support/kb/release-notes/rn-rad-win-vulkan> (search summary; the page timed out).
- Geeks3D, Intel Arc Graphics Driver 32.0.101.89xx (Vulkan report of 32.0.101.8974 for an Arc A750 and an Arrow Lake integrated GPU): <https://www.geeks3d.com/20260815/intel-arc-graphics-driver-32-0-101-89xx/> (fetched).
- Intel, release notes 31.0.101.2141 for 6th to 10th generation graphics: <https://downloadmirror.intel.com/916846/ReleaseNotes_101.2141_WHQL.pdf>, <https://downloadmirror.intel.com/916846/readme.txt> (search summary).
- Intel community, Vulkan extensions missing on Iris Xe: <https://community.intel.com/t5/Graphics/Vulkan-extensions-support-request/m-p/1689173> (search summary).
- Mesamatrix (Mesa's `docs/features.txt` on its main branch, last commit 2026-10-27): <https://mesamatrix.net/> (fetched).
- Phoronix, "Intel Vulkan Linux Driver Lands Graphics Pipeline Library Support": <https://www.phoronix.com/news/Intel-ANV-Graphics-Pipeline-Lib> (search summary).
- Phoronix, "Intel Removes ASTC Hardware From Gen12.5+ Graphics": <https://www.phoronix.com/news/Intel-Gen12.5-No-ASTC>; "Intel Vulkan Driver Lands ASTC LDR Emulation For Latest GPUs": <https://www.phoronix.com/news/Intel-ANV-ASTC-LDR-Emulation> (search summaries).
- DXVK wiki, Driver support: <https://github.com/doitsujin/dxvk/wiki/Driver-support>, read as <https://raw.githubusercontent.com/wiki/doitsujin/dxvk/Driver-support.md> (fetched).
- Khronos, Vulkan 1.4 announcement: <https://www.khronos.org/news/press/khronos-streamlines-development-and-deployment-of-gpu-accelerated-applications-with-vulkan-1.4>; Vulkan 1.4 feature summary: <https://docs.vulkan.org/features/latest/features/proposals/VK_VERSION_1_4.html> (search summaries).
- Khronos blog, "You can use Vulkan without pipelines today" (shader objects and their emulation layer): <https://www.khronos.org/blog/you-can-use-vulkan-without-pipelines-today> (search summary).
