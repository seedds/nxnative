// Draw and render-target state of the NVN replay (see nvn_state.h). The code is nvn_mtl.m's as it was before the move (docs/design/vulkan-backend.md step M2): only the dirty flags
// it set in place became the returned mask, and what touched the Metal backend's own objects became the hooks of nvn_state.h
#include "nvn_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Log lines as the replay layer prints them (nvn_mtl.m MLOG), so a run's log reads as it did before the move
#define SLOG(...) do { fprintf(stderr, "[dm] [nvn-mtl] " __VA_ARGS__); fputc('\n', stderr); } while (0)

nvn_draw_state S;
nvn_rt_state R;
int g_state_inited;

// NVN's BindProgram(program, stage bits) binds per stage: the stages in the bits take that program's shaders (a null program unbinds them), the other stages stay.
// Metroid Dread's depth prepass calls BindProgram(program, vertex | fragment), then BindProgram(null, fragment): only the vertex shader remains and depth is drawn.
// Every bind used to replace S.prog as a whole, a null program meant "no program bound", and the prepass's ~140 draws per frame were all dropped: depth stayed empty, the G-buffer's depth EQUAL test passed no pixel, and scenes and characters were black.
// When every stage comes from the same program, that program is the effective one (all of Mario's and Engage's binds are like this, so nothing changes for them); otherwise a composite is made: a host-made prog_t carrying only a serial, whose NvProg in the serial table takes each stage from its source program (a null fragment = a vertex-only pipeline)
// (the backend makes that object: nvn_state_hook_compose)
const prog_t *nvn_state_bind_stages(const prog_t *p, int stages) {
    for (int st = 0; st < 5; st++) if (stages >> st & 1) S.bst[st] = p;
    const prog_t *v = S.bst[0];
    if (!v) return NULL;
    int same = 1; for (int st = 1; st < 5; st++) if (S.bst[st] && S.bst[st] != v) same = 0;
    if (same && S.bst[1] == v) return v;
    enum { NCMP = 64 };
    static prog_t cp[NCMP]; static const prog_t *ck[NCMP][5]; static uint64_t cs[NCMP][5]; static int ncp, next;
    for (int i = 0; i < ncp; i++) {
        int hit = 1; for (int st = 0; st < 5 && hit; st++) hit = ck[i][st] == S.bst[st] && (!S.bst[st] || cs[i][st] == S.bst[st]->serial);
        if (hit) return &cp[i];
    }
    int i = ncp < NCMP ? ncp++ : next++ % NCMP;
    for (int st = 0; st < 5; st++) { ck[i][st] = S.bst[st]; cs[i][st] = S.bst[st] ? S.bst[st]->serial : 0; }
    uint64_t serial = nvn_state_hook_compose(cp[i].serial, S.bst);   // drops the object of the combination this slot held
    memset(&cp[i], 0, sizeof cp[i]); cp[i].magic = MAGIC('P', 'R', 'O', 'G'); cp[i].serial = serial;
    return &cp[i];
}
// S.prog after S was copied back from a saved state (the precompile prediction, the fusion probe and redraw, deferred draws). A combined program is a slot of the ring above, and binds
// replayed after the save can give the slot the saved pointer names to another combination. AI: THE SOMNIUM FILES (Unity 2017.4) binds its vertex and fragment programs apart; the
// prediction over a 280-draw submission (pso_predict_rest) made more than NCMP combinations, and the draw after it ran Legacy Shaders/VertexLit's shadow vertex shader with
// PSYNC/BG_AlphaTest's shadow pixel shader, a pipeline Metal refused (the pixel shader reads two outputs that vertex shader does not write). The combination is taken again from S.bst
uint32_t nvn_state_prog_resolve(void) {
    const prog_t *np = nvn_state_bind_stages(NULL, 0); if (np == S.prog) return 0;
    S.prog = np;
    static unsigned n; if (!n++ || !(n & (n - 1))) SLOG("combined program taken again after a state restore: its ring slot had been reused (%u times so far)", n);
    return NVN_DIRTY_PIPELINE;
}
uint32_t nvn_state_reset(void) {
    g_state_inited = 1;
    memset(&S, 0, sizeof S);
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD, 0, 0, 0};   // adv, adv_npm, adv_ovl: 0 as before, written out for -Wextra
    S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; S.chmask.mask = 0xffffffffu;
    S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    S.st_vmask[0] = S.st_vmask[1] = S.st_wmask[0] = S.st_wmask[1] = 0xff; S.dr[1] = 1;
    return NVN_DIRTY_PIPELINE | NVN_DIRTY_DEPTH_STENCIL;
}
// A deferred inline update changes what the draws after it read through the uniform bindings it overlaps (they get a shadow): the argument blocks of those stages are encoded again.
// Only those: marking every stage made each later draw encode all of its blocks again, which with the updates kept across passes cost about 1.5 ms a frame in Breath of the Wild.
// Every stage is scanned (the Metal replay skipped the stages already marked; the flags it sets come out the same)
uint32_t nvn_state_ubo_dirty(uint64_t lo, uint64_t hi) {
    uint32_t d = 0;
    for (int st = 0; st < 6; st++)
        for (int r = 0; r < 16; r++) { const p_bind_buf *u = &S.ubo[st][r]; if (u->addr && lo < u->addr + u->size && u->addr < hi) { d |= NVN_DIRTY_ARGS(st); break; } }
    return d;
}

void nvn_state_save(nvn_state_snap *out) { out->s = S; out->r = R; }
uint32_t nvn_state_restore(const nvn_state_snap *in) { S = in->s; R = in->r; return nvn_state_prog_resolve(); }

// GPU bindings belong to a queue, including across submissions. FFX/X-2 destroys its launcher's queue before creating a game's: a global S kept freed launcher programs in S.bst,
// and X-2's first fragment-only bind dereferenced that stale vertex program in keys_hash. Separate queues also retain their own bindings when submissions alternate (Three Houses).
typedef struct queue_state { uint64_t serial; nvn_state_snap snap; struct queue_state *next; } queue_state;
static queue_state *g_queue_states, *g_active_queue;
uint32_t nvn_state_queue_activate(uint64_t serial) {   // caller holds the submission lock
    if (g_active_queue && g_active_queue->serial == serial) return 0;
    if (g_active_queue) nvn_state_save(&g_active_queue->snap);
    queue_state *q = g_queue_states;
    while (q && q->serial != serial) q = q->next;
    if (q) nvn_state_restore(&q->snap);
    else {
        q = calloc(1, sizeof *q); if (!q) { SLOG("FATAL queue state allocation failed"); abort(); }
        q->serial = serial; q->next = g_queue_states; g_queue_states = q;
        nvn_state_reset(); memset(&R, 0, sizeof R);
    }
    g_active_queue = q; return NVN_DIRTY_PIPELINE | NVN_DIRTY_DEPTH_STENCIL | NVN_DIRTY_ARGS_ALL;
}
uint32_t nvn_state_queue_forget(uint64_t serial) {   // caller holds the submission lock
    queue_state **p = &g_queue_states;
    while (*p && (*p)->serial != serial) p = &(*p)->next;
    if (!*p) return 0;
    queue_state *q = *p; *p = q->next; uint32_t d = 0;
    if (g_active_queue == q) { g_active_queue = NULL; d = nvn_state_reset(); memset(&R, 0, sizeof R); d |= NVN_DIRTY_ARGS_ALL; }
    free(q);
    return d;
}

uint32_t nvn_state_apply(uint32_t op, const void *a) {
    uint32_t d = 0;
    switch (op) {
        case OP_SCISSOR: R.sc = *(const p_scissor *)a; R.has_sc = 1; break;
        // The pool bound when the draw runs, as on the GPU (a command buffer that binds none keeps the queue's): handles a shader reads from a uniform buffer are resolved only here,
        // so they take this base. Dark Souls Remastered keeps 12 sampler pools with different samplers under the same IDs and its map materials read their handles from uniform
        // memory; resolved in the first pool, the walls' tiling textures took a clamp-to-edge sampler and smeared sideways
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: { int tex = op == OP_SET_TEX_POOL, b = nvn_dpool_base(((const p_set_pool *)a)->pool, tex);
            if (b != (tex ? S.tex_base : S.samp_base)) { if (tex) S.tex_base = b; else S.samp_base = b; d |= NVN_DIRTY_ARGS_ALL; }
            break; }
        // The compute stage has a binding of its own: a compute bind used to replace the one bound program, so a draw after a dispatch found the compute program (or none) and was
        // dropped, and a dispatch after a graphics bind found no compute shader. Hyrule Warriors: Age of Calamity interleaves them; its movie's NV12 → RGB draw was dropped and the intro played black
        case OP_BIND_PROGRAM: { const p_bind_program *bp_ = a;
            if ((bp_->stages & 0x20) && bp_->prog != S.cprog) { S.cprog = bp_->prog; d |= NVN_DIRTY_ARGS_ALL; }
            if (bp_->stages & 0x1f) { const prog_t *np = nvn_state_bind_stages(bp_->prog, bp_->stages & 0x1f); if (np != S.prog) { S.prog = np; d |= NVN_DIRTY_PIPELINE | NVN_DIRTY_ARGS_ALL; } }
            break; }
        case OP_BIND_STATE: {
            const p_bind_state *b = a; void *dst = NULL; size_t n = 0;
            blend_st bs;
            switch (b->kind) {
                case 0: memcpy(&bs, b->bytes, sizeof bs); dst = &S.blend[bs.target]; n = sizeof bs; break;
                case 1: dst = &S.color; n = sizeof S.color; break;
                case 2: dst = &S.chmask; n = sizeof S.chmask; break;
                case 3: dst = &S.ms; n = sizeof S.ms; break;
                case 4: dst = &S.poly; n = sizeof S.poly; break;
                case 5: dst = &S.ds; n = sizeof S.ds; break;
            }
            if (dst && memcmp(dst, b->bytes, n)) { memcpy(dst, b->bytes, n); if (b->kind == 5) d |= NVN_DIRTY_DEPTH_STENCIL; else d |= NVN_DIRTY_PIPELINE; }
            break;
        }
        case OP_BIND_VATTR: { const p_bind_vattr *b = a; if (S.nattr != b->n || memcmp(S.attr, b->st, 4 * b->n)) { memcpy(S.attr, b->st, 4 * b->n); S.nattr = b->n; d |= NVN_DIRTY_PIPELINE; } break; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; if (S.nstream != b->n || memcmp(S.stream, b->st, 8 * b->n)) { memcpy(S.stream, b->st, 8 * b->n); S.nstream = b->n; d |= NVN_DIRTY_PIPELINE; } break; }
        case OP_BIND_VBUF: case OP_BIND_VBUF1: { p_bind_vbuf tmp; const p_bind_vbuf *b = vbuf_rec(op, a, &tmp); memcpy(&S.vbuf[b->first], b->r, sizeof *b->r * b->n); break; }
        case OP_BIND_UBO: case OP_BIND_UBO_SMALL: { p_bind_buf tmp; const p_bind_buf *b = bind_buf_rec(op, a, &tmp); if (b->stage < 6 && b->index < 16 && memcmp(&S.ubo[b->stage][b->index], b, sizeof *b)) { S.ubo[b->stage][b->index] = *b; d |= NVN_DIRTY_ARGS(b->stage); } break; }
        case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index < 16 && memcmp(&S.ssbo[b->stage][b->index], b, sizeof *b)) { S.ssbo[b->stage][b->index] = *b; d |= NVN_DIRTY_ARGS(b->stage); } break; }
        case OP_BIND_TEX: case OP_BIND_TEX1: { p_bind_handles t1; const p_bind_handles *b = bind_rec(op, a, &t1);
            if (b->n < 0 || b->n > 32) { nvn_state_hook_bind_bad(a, b->n); break; }   // the recorder never writes more than 32 (bind_handles): the record was overwritten after it was recorded
            nvn_state_hook_bind(op, b);
            if (b->stage < 6 && b->first + b->n <= NTEX && memcmp(&S.tex[b->stage][b->first], b->h, 8 * b->n)) { memcpy(&S.tex[b->stage][b->first], b->h, 8 * b->n); d |= NVN_DIRTY_ARGS(b->stage); } break; }
        case OP_BIND_SAMP: { const p_bind_handles *b = a; nvn_state_hook_bind(op, b); if (b->stage < 6 && b->first + b->n <= 128 && memcmp(&S.samp[b->stage][b->first], b->h, 8 * b->n)) { memcpy(&S.samp[b->stage][b->first], b->h, 8 * b->n); d |= NVN_DIRTY_ARGS(b->stage); } break; }
        case OP_BIND_IMG: { const p_bind_handles *b = a; nvn_state_hook_bind(op, b); if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 32) { if (memcmp(&S.img[b->stage][b->first], b->h, 8 * b->n)) { memcpy(&S.img[b->stage][b->first], b->h, 8 * b->n); d |= NVN_DIRTY_ARGS(b->stage); } } else nvn_state_hook_img_range(b); break; }
        case OP_STENCIL: {
            const p_stencil *b = a;
            if (b->which != 2) d |= NVN_DIRTY_DEPTH_STENCIL;
            for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) S.st_vmask[f] = b->value_mask; else if (b->which == 1) S.st_wmask[f] = b->mask; else S.st_ref[f] = b->ref; }
            break;
        }
        case OP_POLY_OFFSET: S.po = *(const p_poly_offset *)a; break;
        case OP_VIEWPORTS: { const p_viewports *b = a; for (int i = 0; i < b->n; i++) memcpy(S.vp[b->first + i], b->v[i], 16); S.has_vp = 1; break; }
        case OP_DEPTH_RANGE: { const p_depth_range *b = a; S.dr[0] = b->n; S.dr[1] = b->f; break; }
        case OP_DEPTH_CLAMP: S.depth_clamp = ((const p_depth_clamp *)a)->on; break;
        case OP_VP_SWIZZLE: { const p_vp_swizzle *b = a; memcpy(S.vp_sw[b->first], b->sw, 4 * (size_t)b->n); break; }
        case OP_BLEND_COLOR: memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
        case OP_COPY_STRIDE: { const p_copy_stride *cs = a; if (cs->image) S.copy_img_stride = cs->stride; else S.copy_stride = cs->stride; break; }
        case OP_TF_BIND: { const p_tf_bind *b = a; if (b->index >= 0 && b->index < 4) S.tf[b->index] = *b; break; }
        case OP_TF_STATE: { const p_tf_state *b = a; S.tf_on = b->what == 0 || b->what == 3; break; }
        case OP_PRIM_RESTART: S.restart = *(const p_prim_restart *)a; break;
        case OP_RAST_DISCARD: S.rast_discard = ((const p_rast_discard *)a)->on; d |= NVN_DIRTY_PIPELINE; break;
        default: return 0;
    }
    return d | NVN_STATE_APPLIED;
}
