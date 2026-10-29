// The NVN command replay's decoder (nvn_replay.h): the decode loop of the Metal replay layer's mtl_exec, moved with no change of logic (docs/design/vulkan-backend.md, step M3).
// What each case arm did in the backend is the backend's per-operation function (nvn_be.h; the Metal backend's are mtl_exec's arms verbatim, nvn_mtl.m); the state records are
// applied here, and their dirty mask is handed to be_state_dirty exactly where the Metal arm called state_op, before or after the arm's own work as it was
#include <string.h>
#include "nvn_replay.h"
#include "nvn_be.h"
#include "nvn_state.h"

const uint8_t *nvn_replay_ret[NVN_REPLAY_RET_MAX]; int nvn_replay_ret_n;

void nvn_replay_exec(const uint8_t *p) {
    if (!g_state_inited) be_state_dirty(nvn_state_reset());
    int list = be_list_begin();
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (be_rec_pre(p, &h, list)) { p += h.size; continue; }   // consumed by the backend's look-ahead (Metal: a pure-clear pass already executed early, hoist_take)
        const void *a = p + 8;
        switch (h.op) {
            case OP_END: be_end(p, &h, a); return;
            case OP_BEGIN: break;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8);
                if (nvn_replay_ret_n < NVN_REPLAY_RET_MAX) nvn_replay_ret[nvn_replay_ret_n] = p + h.size;   // Return stack for lookahead: after a sub-list ends, continue looking at the caller
                nvn_replay_ret_n++;
                nvn_replay_exec((const uint8_t *)t); nvn_replay_ret_n--; break; }
            case OP_SIGNAL_EVENT: be_signal_event(p, &h, a); break;
            case OP_REPORT_COUNTER: be_report_counter(p, &h, a); break;
            case OP_RENDER_COND: be_render_cond(p, &h, a); break;
            case OP_FENCE_SYNC: be_fence_sync(p, &h, a); break;
            case OP_RESET_COUNTER: be_reset_counter(p, &h, a); break;
            case OP_CLEAR_BUF: be_fill(p, &h, a); break;
            case OP_COPY_B2B: be_copy_b2b(p, &h, a); break;
            case OP_UPDATE_UB: be_update_ub(p, &h, a); break;
            case OP_SET_RT: be_set_rt(p, &h, a); break;
            case OP_CLEAR_COLOR: be_clear_color(p, &h, a); break;
            case OP_CLEAR_DS: be_clear_ds(p, &h, a); break;
            case OP_DRAW: be_draw(p, &h, a); break;
            case OP_DRAW_IDX: be_draw_idx(p, &h, a); break;
            // Do not mark dirty if the state did not change (the game often re-sends the same program / state block / vertex format before each draw; the dirty flag only decides whether to re-fetch the pipeline / depth-stencil object, and equal values mean the same object)
            case OP_BIND_PROGRAM: be_state_dirty(nvn_state_apply(h.op, a)); be_bind_program(p, &h, a); break;
            case OP_SCISSOR: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_VBUF1:
            case OP_BIND_UBO: case OP_BIND_UBO_SMALL: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL:
            case OP_POLY_OFFSET: case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_DEPTH_CLAMP: case OP_VP_SWIZZLE: case OP_BLEND_COLOR: case OP_COPY_STRIDE:
                be_state_dirty(nvn_state_apply(h.op, a)); break;
            case OP_TF_BIND: be_tf_bind(p, &h, a); be_state_dirty(nvn_state_apply(h.op, a)); break;
            case OP_TF_STATE: be_tf_state_note(p, &h, a); be_state_dirty(nvn_state_apply(h.op, a)); be_tf_state(p, &h, a); break;
            case OP_PRIM_RESTART: be_state_dirty(nvn_state_apply(h.op, a)); break;
            case OP_RAST_DISCARD: be_rast_discard(p, &h, a); be_state_dirty(nvn_state_apply(h.op, a)); break;
            case OP_COPY_B2T: be_copy_b2t(p, &h, a); break;
            case OP_COPY_T2B: be_copy_t2b(p, &h, a); break;
            case OP_COPY_T2T: be_copy_t2t(p, &h, a); break;
            case OP_CLEAR_TEX: be_clear_tex(p, &h, a); break;
            case OP_DISPATCH: be_dispatch(p, &h, a); break;
            case OP_BARRIER: be_barrier(p, &h, a); break;
            case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: be_state_dirty(nvn_state_apply(h.op, a)); break;
            default: break;
        }
        p += h.size;
    }
}
