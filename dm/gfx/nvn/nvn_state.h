// Draw and render-target state of the NVN replay, shared by every graphics backend (pure C, every host): the state records of a command stream applied to S and R, the per-stage
// program binds, and each queue's own copy of the state. Moved out of the Metal replay layer (nvn_mtl.m) with no change of logic (docs/design/vulkan-backend.md, sections 1.3 and 1.7,
// step M2). A backend reads S and R directly and turns the dirty mask the functions below return into its own flags (nvn_mtl.m state_dirty: g_pso_dirty, g_dss_dirty, g_args_dirty).
// Called on the submit thread only, under hle_nvn.c's submission lock, as before the move
#pragma once
#include <stddef.h>   // offsetof: a backend checks nvn_clear_color against its own type (nvn_mtl.m)
#include <stdint.h>
#include "nvn_obj.h"

// Texture slot table per stage: 0-127 the combined handles (nvnCommandBufferBindTexture), 128-255 the separate texture binds (NVN_SEPTEX_SLOT0, nvn_obj.h)
#define NTEX 256

// A colour target's clear colour, laid out as Metal's MTLClearColor (same field names and order): the Metal backend converts it where Metal takes one (nvn_mtl.m cc_mtl)
typedef struct { double red, green, blue, alpha; } nvn_clear_color;

// ---------- Draw state ----------
typedef struct {
    const prog_t *prog;
    const prog_t *cprog;       // compute program: NVN binds it apart from the graphics stages (see OP_BIND_PROGRAM in nvn_state_apply)
    p_prim_restart restart;
    const prog_t *bst[5];      // program bound per stage (NVN stage bits 1 vertex 2 fragment 4 geometry 8 / 16 tessellation, indices as in NvProg.sh), see nvn_state_bind_stages
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[16]; int nattr; vs_st stream[16]; int nstream;
    NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t tex[6][NTEX], samp[6][128], img[6][32];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][4]; float dr[2]; int has_vp; int depth_clamp; uint8_t vp_sw[16][4];   // vp_sw: viewport swizzles XOR the identity (p_vp_swizzle)
    float blend_color[4]; int64_t copy_stride, copy_img_stride;
    p_tf_bind tf[4]; int tf_on, rast_discard;   // capture buffers and enables; byte offsets and captured count stay on the GPU (nvn_feedback.inc)
    int tex_base, samp_base;   // host-table bases of the texture / sampler pool bound (OP_SET_TEX_POOL / OP_SET_SAMP_POOL), for handles read at replay from uniform buffers (encode_args)
} nvn_draw_state;

// ---------- Render-target state: SetRenderTargets opens a new pass; clears before the first command in the pass are folded into the load action ----------
typedef struct {
    p_set_rt rt; int has_rt;
    uint8_t clear_c[8]; nvn_clear_color cc[8];
    uint8_t clear_d, clear_s; double cd; uint32_t cs;
    p_scissor sc; int has_sc;
} nvn_rt_state;

extern nvn_draw_state S;
extern nvn_rt_state R;
extern int g_state_inited;   // nvn_state_reset has run (the replay resets the state before its first record otherwise, mtl_exec)

// ---------- Dirty mask: what a state change asks the backend to fetch or encode again ----------
#define NVN_DIRTY_PIPELINE      0x1u          // pipeline-related state (Metal: g_pso_dirty)
#define NVN_DIRTY_DEPTH_STENCIL 0x2u          // depth-stencil state (Metal: g_dss_dirty)
#define NVN_DIRTY_ARGS(stage)   (0x4u << (stage))   // that NVN stage's (0-5) argument blocks are encoded again (Metal: g_args_dirty[stage])
#define NVN_DIRTY_ARGS_ALL      (0x3fu << 2)  // every stage's (Metal: args_dirty_all)
#define NVN_STATE_APPLIED       0x80000000u   // nvn_state_apply: the record was a state record (set even when nothing changed)

// Applies one state record (op and its payload, as mtl_exec reads them): 0 when op is not a state record, else NVN_STATE_APPLIED | the dirty bits
uint32_t nvn_state_apply(uint32_t op, const void *payload);
// NVN's BindProgram(program, stage bits) for the graphics stages: the program S.prog takes after binding p to the stages in the bits (see nvn_state.c)
const prog_t *nvn_state_bind_stages(const prog_t *p, int stages);
// S.prog taken again from S.bst after S was copied back from a saved state: NVN_DIRTY_PIPELINE when it changed, else 0
uint32_t nvn_state_prog_resolve(void);
// S to NVN's defaults (sets g_state_inited; R is left as it is): the dirty bits
uint32_t nvn_state_reset(void);
// The stages whose uniform bindings overlap [lo, hi) (a deferred inline update there changes what their draws read): NVN_DIRTY_ARGS bits
uint32_t nvn_state_ubo_dirty(uint64_t lo, uint64_t hi);

// ---------- A copy of the state: each queue keeps its own ----------
typedef struct { nvn_draw_state s; nvn_rt_state r; } nvn_state_snap;
void nvn_state_save(nvn_state_snap *out);
uint32_t nvn_state_restore(const nvn_state_snap *in);   // the dirty bits of nvn_state_prog_resolve
// The submission about to be replayed belongs to this queue (be_queue_activate): the active queue's state is saved and this one's restored, or started from the defaults.
// 0 when it is the active queue already, else every dirty bit. Caller holds the submission lock
uint32_t nvn_state_queue_activate(uint64_t serial);
// The queue is finalized (be_queue_forget): its state goes; when it is the active one, S and R start again from the defaults (every dirty bit), else 0
uint32_t nvn_state_queue_forget(uint64_t serial);

// ---------- Provided by others ----------
int nvn_dpool_base(const void *pool, int tex);   // hle_nvn.c: where a descriptor pool's IDs start in the host table
// Hooks the backend defines (nvn_mtl.m), for the parts of a state record's effect that live in the backend
// A combined program (see nvn_state_bind_stages) is made for the stages in bst: drop the backend object of old_serial (0 = none), make one that takes each stage from the program bound
// there, and return its new serial (nvn_new_serial)
uint64_t nvn_state_hook_compose(uint64_t old_serial, const prog_t *const bst[5]);
// Before a texture, sampler or image bind is applied: deferred handles are read again (the record is updated in place), and the backend's diagnostics see the bind (op: the record's)
void nvn_state_hook_bind(uint32_t op, const p_bind_handles *b);
// A texture bind record whose handle count is out of range (overwritten after it was recorded): reported, and the bind is skipped
void nvn_state_hook_bind_bad(const void *rec, int n);
// An image bind whose slots are out of range: reported, and the bind is skipped
void nvn_state_hook_img_range(const p_bind_handles *b);
