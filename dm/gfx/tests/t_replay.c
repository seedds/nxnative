// Unit tests of the NVN command replay's decoder (nvn_replay.c), CPU only, on every host: the walk over a command list's records (straight, OP_JUMP to another block, OP_CALL into
// sub-lists that end and return to the record after the CALL, nested to the return stack's depth and one past it), records the backend consumes in be_rec_pre, every opcode's
// backend calls, and where the state records are applied (the real nvn_state.c) relative to the backend's own work. The backend here is a stub that records every call: its name,
// the record and header it got, the dirty mask, the call depth and stored return records, the list value of be_list_begin. Expected values are written out from the Metal replay's
// loop as it was before the move (nvn_mtl.m mtl_exec at 1aff12a), not from the decoder's source
#include "../nvn/nvn_replay.h"
#include "../nvn/nvn_be.h"
#include "../nvn/nvn_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail, total;
static void t(const char *name, int ok) {
    total++; fail += !ok;
    printf("%-78s %s\n", name, ok ? "✓" : "✗");
}

// ---------- Stubs: hle_nvn.c and the state hooks of the backend ----------
static dpool_t pool_a;
int nvn_dpool_base(const void *pool, int tex) { return pool == &pool_a ? (tex ? 100 : 200) : 0; }
uint64_t nvn_state_hook_compose(uint64_t old_serial, const prog_t *const bst[5]) { (void)old_serial; (void)bst; return 900; }
static int n_hook_bind;
void nvn_state_hook_bind(uint32_t op, const p_bind_handles *b) { (void)op; (void)b; n_hook_bind++; }
void nvn_state_hook_bind_bad(const void *rec, int n) { (void)rec; (void)n; }
void nvn_state_hook_img_range(const p_bind_handles *b) { (void)b; }

// ---------- The stub backend: every call is an event ----------
typedef struct {
    const char *fn; const uint8_t *rec; uint32_t op, size; int a_ok;   // a_ok: the payload pointer is rec + 8
    uint32_t mask; int depth; const uint8_t *ret[NVN_REPLAY_RET_MAX]; int list;
    int tf_on, rast_discard; const prog_t *prog; p_tf_bind tf0;   // the draw state when the backend was called
} call_ev;
enum { NEV = 4096 };
static call_ev ev[NEV]; static int nev;
static int next_list = 1;   // be_list_begin hands out 1, 2, 3, ...
static const uint8_t *consume[8]; static int nconsume;   // records be_rec_pre consumes
static call_ev *note(const char *fn, const uint8_t *rec, const op_hdr *h, const void *a) {
    call_ev *e = &ev[nev < NEV ? nev++ : NEV - 1]; memset(e, 0, sizeof *e);
    e->fn = fn; e->rec = rec; e->op = h ? h->op : ~0u; e->size = h ? h->size : 0; e->a_ok = a == (const void *)(rec + 8);
    e->depth = nvn_replay_ret_n; memcpy(e->ret, nvn_replay_ret, sizeof e->ret);
    e->tf_on = S.tf_on; e->rast_discard = S.rast_discard; e->prog = S.prog; e->tf0 = S.tf[0];
    return e;
}
int be_list_begin(void) { call_ev *e = note("list_begin", NULL, NULL, NULL); e->list = next_list++; return e->list; }
int be_rec_pre(const uint8_t *rec, const op_hdr *h, int list) {
    call_ev *e = note("pre", rec, h, rec + 8); e->list = list;
    for (int i = 0; i < nconsume; i++) if (consume[i] == rec) return 1;
    return 0;
}
void be_state_dirty(uint32_t mask) { call_ev *e = note("dirty", NULL, NULL, NULL); e->mask = mask; }
#define STUB(name) void be_##name(const uint8_t *rec, const op_hdr *h, const void *a) { note(#name, rec, h, a); }
STUB(end) STUB(signal_event) STUB(report_counter) STUB(render_cond) STUB(fence_sync) STUB(reset_counter) STUB(fill) STUB(copy_b2b) STUB(update_ub) STUB(set_rt)
STUB(clear_color) STUB(clear_ds) STUB(draw) STUB(draw_idx) STUB(bind_program) STUB(tf_bind) STUB(tf_state_note) STUB(tf_state) STUB(rast_discard) STUB(copy_b2t)
STUB(copy_t2b) STUB(copy_t2t) STUB(clear_tex) STUB(dispatch) STUB(barrier)

// ---------- Command lists in memory ----------
typedef struct { uint64_t w[2048]; size_t n; } list_t;   // 8-byte aligned records
static const uint8_t *emit(list_t *l, uint32_t op, const void *payload, size_t len) {
    uint8_t *p = (uint8_t *)l->w + l->n; op_hdr h = {op, (uint32_t)(8 + ((len + 7) & ~(size_t)7))};
    memcpy(p, &h, 8); if (len) memcpy(p + 8, payload, len);
    l->n += h.size; return p;
}
static const uint8_t *emit0(list_t *l, uint32_t op) { uint64_t z[2] = {0}; return emit(l, op, z, 16); }   // a record whose payload the stub backend does not read
static const uint8_t *emit_ptr(list_t *l, uint32_t op, const void *target) { uint64_t v = (uint64_t)(uintptr_t)target; return emit(l, op, &v, 8); }

static void reset_ev(void) { nev = 0; nconsume = 0; }
// The events from i on, as a space-separated string of names (pre events as "pre:<op>")
static const char *seq(int from) {
    static char s[8192]; size_t k = 0; s[0] = 0;
    for (int i = from; i < nev && k < sizeof s - 64; i++) {
        if (!strcmp(ev[i].fn, "pre")) k += (size_t)snprintf(s + k, sizeof s - k, "%spre:%u", k ? " " : "", ev[i].op);
        else k += (size_t)snprintf(s + k, sizeof s - k, "%s%s", k ? " " : "", ev[i].fn);
    }
    return s;
}
static int find(const char *fn, int from) { for (int i = from; i < nev; i++) if (!strcmp(ev[i].fn, fn)) return i; return -1; }
static int find_pre(const uint8_t *rec) { for (int i = 0; i < nev; i++) if (!strcmp(ev[i].fn, "pre") && ev[i].rec == rec) return i; return -1; }
// The mask be_state_dirty got while the record at r was handled (-1: none)
static int mask_after(const uint8_t *r) {
    int p = find_pre(r); if (p < 0) return -1;
    for (int j = p + 1; j < nev && strcmp(ev[j].fn, "pre"); j++) if (!strcmp(ev[j].fn, "dirty")) return (int)ev[j].mask;
    return -1;
}

static list_t L[20];
static prog_t P[2];

int main(void) {
    char want[512];
    for (int i = 0; i < 2; i++) { P[i].magic = MAGIC('P', 'R', 'O', 'G'); P[i].serial = 11 + (uint64_t)i; }

    // ---------- A straight list; the state is reset before the first record ----------
    list_t *a = &L[0]; memset(a, 0, sizeof *a);
    const uint8_t *r_rt = emit0(a, OP_SET_RT), *r_cc = emit0(a, OP_CLEAR_COLOR), *r_dw = emit0(a, OP_DRAW), *r_end = emit0(a, OP_END);
    reset_ev(); g_state_inited = 0;
    nvn_replay_exec((const uint8_t *)a->w);
    snprintf(want, sizeof want, "dirty list_begin pre:%u set_rt pre:%u clear_color pre:%u draw pre:%u end", OP_SET_RT, OP_CLEAR_COLOR, OP_DRAW, OP_END);
    t("straight list: reset, list start, each record's pre then its function", !strcmp(seq(0), want));
    t("the first replay resets the state: its mask (pipeline and depth-stencil) to be_state_dirty", ev[0].mask == (NVN_DIRTY_PIPELINE | NVN_DIRTY_DEPTH_STENCIL) && g_state_inited == 1);
    t("pre and the function get the record, its header and payload", ev[2].rec == r_rt && ev[3].rec == r_rt && ev[3].op == OP_SET_RT && ev[3].size == 24 && ev[3].a_ok && ev[2].a_ok);
    t("records in order: clear, draw, end", ev[5].rec == r_cc && ev[7].rec == r_dw && ev[9].rec == r_end && ev[9].op == OP_END);
    t("every record of the list carries the list's value", ev[2].list == ev[1].list && ev[8].list == ev[1].list && ev[1].list == 1);
    t("outside a call: depth 0 at the end", ev[9].depth == 0 && nvn_replay_ret_n == 0);
    reset_ev();
    nvn_replay_exec((const uint8_t *)a->w);
    t("a second replay: no reset (the state is set)", !strcmp(ev[0].fn, "list_begin") && find("dirty", 0) < 0 && ev[0].list == 2);

    // ---------- JUMP to another block: the same list (one list start), the jump has no backend call ----------
    list_t *b = &L[1], *c = &L[2]; memset(b, 0, sizeof *b); memset(c, 0, sizeof *c);
    const uint8_t *c_dp = emit0(c, OP_DISPATCH), *c_end = emit0(c, OP_END);
    const uint8_t *b_dw = emit0(b, OP_DRAW), *b_j = emit_ptr(b, OP_JUMP, c->w);
    emit0(b, OP_DRAW);   // after the jump: never reached
    reset_ev();
    nvn_replay_exec((const uint8_t *)b->w);
    snprintf(want, sizeof want, "list_begin pre:%u draw pre:%u pre:%u dispatch pre:%u end", OP_DRAW, OP_JUMP, OP_DISPATCH, OP_END);
    t("jump: draw, the jump's pre, then the target block, one list start", !strcmp(seq(0), want));
    t("jump: records walked are the draw, the jump, the target's dispatch and end", ev[1].rec == b_dw && ev[3].rec == b_j && ev[4].rec == c_dp && ev[6].rec == c_end);
    t("jump: the target's records keep the list's value", ev[4].list == ev[0].list && ev[6].list == ev[0].list);
    t("jump: the record after the jump is not reached", find_pre(b_j + 8 + 8) < 0);

    // ---------- CALL into a sub-list that ENDs: the caller goes on after the CALL ----------
    list_t *m = &L[3], *s = &L[4]; memset(m, 0, sizeof *m); memset(s, 0, sizeof *s);
    const uint8_t *s_dp = emit0(s, OP_DISPATCH), *s_end = emit0(s, OP_END);
    const uint8_t *m_dw = emit0(m, OP_DRAW), *m_call = emit_ptr(m, OP_CALL, s->w), *m_bar = emit0(m, OP_BARRIER), *m_end = emit0(m, OP_END);
    reset_ev();
    nvn_replay_exec((const uint8_t *)m->w);
    snprintf(want, sizeof want, "list_begin pre:%u draw pre:%u list_begin pre:%u dispatch pre:%u end pre:%u barrier pre:%u end", OP_DRAW, OP_CALL, OP_DISPATCH, OP_END, OP_BARRIER, OP_END);
    t("call: draw, the call's pre, the sub-list (its own start) to its end, then the caller", !strcmp(seq(0), want));
    int i_sdp = find_pre(s_dp), i_send = find_pre(s_end), i_bar = find_pre(m_bar), i_mend = find_pre(m_end);
    t("call: in the sub-list depth 1, the return record is the one after the CALL", ev[i_sdp].depth == 1 && ev[i_sdp].ret[0] == m_call + 16 && m_call + 16 == m_bar);
    t("call: the sub-list's END is seen at depth 1 (pre and be_end)", ev[i_send].depth == 1 && ev[i_send + 1].depth == 1 && !strcmp(ev[i_send + 1].fn, "end"));
    t("call: back in the caller at depth 0", ev[i_bar].depth == 0 && ev[i_mend].depth == 0 && nvn_replay_ret_n == 0);
    t("call: the sub-list has its own list value, the caller's comes back after", ev[i_sdp].list == ev[i_send].list && ev[i_sdp].list != ev[0].list && ev[i_bar].list == ev[0].list);
    t("call: the draw before it at depth 0", ev[find_pre(m_dw)].depth == 0);

    // ---------- Nested CALLs: 16 deep (all stored) and 17 deep (the 17th counted, not stored) ----------
    for (int depth = 16; depth <= 17; depth++) {
        const uint8_t *after_call[20], *inner_dw, *list_end[20];
        for (int i = 0; i <= depth; i++) memset(&L[i], 0, sizeof L[i]);
        for (int i = 0; i < depth; i++) { const uint8_t *cl = emit_ptr(&L[i], OP_CALL, L[i + 1].w); after_call[i] = cl + 16; list_end[i] = emit0(&L[i], OP_END); }
        inner_dw = emit0(&L[depth], OP_DRAW); list_end[depth] = emit0(&L[depth], OP_END);
        reset_ev();
        nvn_replay_exec((const uint8_t *)L[0].w);
        int id = find_pre(inner_dw); const call_ev *e = &ev[id];
        int stored = 1; for (int i = 0; i < 16; i++) stored &= e->ret[i] == after_call[i] && after_call[i] == list_end[i];
        char name[96];
        snprintf(name, sizeof name, "%d nested calls: the innermost draw at depth %d", depth, depth);
        t(name, id >= 0 && e->depth == depth && !strcmp(ev[id + 1].fn, "draw"));
        snprintf(name, sizeof name, "%d nested calls: the first 16 return records stored, outermost first", depth);
        t(name, stored);
        if (depth == 17) {
            t("17 deep: the 17th caller's return is not stored (slot 15 keeps the 16th's)", e->ret[15] == after_call[15] && e->ret[15] != after_call[16]);
            int i16 = find_pre(list_end[16]);
            t("17 deep: back in the 17th caller, depth 16 and the stored records unchanged", i16 >= 0 && ev[i16].depth == 16 && ev[i16].ret[15] == after_call[15]);
        }
        int ends = 0; for (int i = 0; i < nev; i++) ends += !strcmp(ev[i].fn, "end");
        int starts = 0; for (int i = 0; i < nev; i++) starts += !strcmp(ev[i].fn, "list_begin");
        snprintf(name, sizeof name, "%d nested calls: every list starts and ends once, depth 0 after", depth);
        t(name, ends == depth + 1 && starts == depth + 1 && nvn_replay_ret_n == 0);
        int i0 = find_pre(list_end[0]);
        snprintf(name, sizeof name, "%d nested calls: the outermost END at depth 0", depth);
        t(name, i0 >= 0 && ev[i0].depth == 0 && ev[i0].list == ev[0].list);
    }

    // ---------- Records consumed by be_rec_pre: the body is skipped and the walk goes on ----------
    list_t *k = &L[5], *ks = &L[6]; memset(k, 0, sizeof *k); memset(ks, 0, sizeof *ks);
    emit0(ks, OP_DISPATCH); emit0(ks, OP_END);
    p_bind_state bs; memset(&bs, 0, sizeof bs); bs.kind = 4; bs.bytes[0] = 1;   // polygon state: cull front (differs from the defaults)
    nvn_state_reset();
    poly_st poly0 = S.poly;
    const uint8_t *k_dw = emit0(k, OP_DRAW), *k_bs = emit(k, OP_BIND_STATE, &bs, sizeof bs), *k_call = emit_ptr(k, OP_CALL, ks->w), *k_end1 = emit0(k, OP_END);
    const uint8_t *k_cc = emit0(k, OP_CLEAR_COLOR), *k_end2 = emit0(k, OP_END);
    reset_ev(); consume[nconsume++] = k_dw; consume[nconsume++] = k_bs; consume[nconsume++] = k_call; consume[nconsume++] = k_end1;
    nvn_replay_exec((const uint8_t *)k->w);
    snprintf(want, sizeof want, "list_begin pre:%u pre:%u pre:%u pre:%u pre:%u clear_color pre:%u end", OP_DRAW, OP_BIND_STATE, OP_CALL, OP_END, OP_CLEAR_COLOR, OP_END);
    t("consumed draw, state, call and end: only their pre; the walk goes on", !strcmp(seq(0), want));
    t("consumed: each next record is the consumed one + its size", ev[2].rec == k_dw + 24 && ev[3].rec == k_bs + ((const op_hdr *)k_bs)->size && ev[4].rec == k_call + 16 && ev[5].rec == k_end1 + 24);
    t("consumed state record: not applied (no dirty, S unchanged)", !memcmp(&S.poly, &poly0, sizeof poly0) && find("dirty", 0) < 0);
    t("consumed call: no sub-list started", find("dispatch", 0) < 0 && find("list_begin", 1) < 0);
    t("the walk ends at the END that is not consumed", ev[nev - 2].rec == k_end2 && ev[nev - 1].rec == k_end2 && ev[6].rec == k_cc);

    // ---------- Every opcode: the backend calls of each, and the state applied around them ----------
    nvn_state_reset(); memset(&R, 0, sizeof R);
    list_t *o = &L[7]; memset(o, 0, sizeof *o);
    p_scissor sc = {1, 2, 30, 40};
    p_bind_program bp = {&P[0], 3};
    p_bind_state bds; memset(&bds, 0, sizeof bds); bds.kind = 5; bds.bytes[0] = 1;   // depth test on: depth-stencil dirty
    p_bind_vattr va; memset(&va, 0, sizeof va); va.n = 1; va.st[0] = 5;
    p_bind_vstream vs; memset(&vs, 0, sizeof vs); vs.n = 1; vs.st[0].stride = 16;
    p_bind_vbuf vb; memset(&vb, 0, sizeof vb); vb.n = 1; vb.r[0].address = 0x1000; vb.r[0].size = 64;
    p_bind_vbuf1 vb1 = {2, 32, 0x2000};
    p_bind_buf ubo = {2, 3, 0x3000, 256}, ssbo = {4, 1, 0x4000, 128};
    p_bind_ubo_small ubs = {1, 5, 64, 0x5000};
    p_bind_handles bt; memset(&bt, 0, sizeof bt); bt.stage = 1; bt.first = 0; bt.n = 1; bt.h[0] = 7;
    p_bind_tex1 bt1 = {0, 3, 9, 0, 0};
    p_bind_handles bsm; memset(&bsm, 0, sizeof bsm); bsm.stage = 1; bsm.first = 0; bsm.n = 1; bsm.h[0] = 3;
    p_bind_handles bim; memset(&bim, 0, sizeof bim); bim.stage = 1; bim.first = 0; bim.n = 1; bim.h[0] = 4;
    p_stencil st = {3, 0x0f, 0, 0, 0};
    p_poly_offset po = {1, 2, 3};
    p_viewports vp; memset(&vp, 0, sizeof vp); vp.n = 1; vp.v[0][2] = 1280;
    p_depth_range dr = {0.25f, 0.75f};
    p_depth_clamp dc = {1};
    p_vp_swizzle sw; memset(&sw, 0, sizeof sw); sw.n = 1; sw.sw[0][0] = 2;
    p_blend_color bc = {{0.5f, 0, 0, 1}};
    p_copy_stride cs = {256, 0};
    p_tf_bind tfb = {0, 0, 0x6000, 512};
    p_tf_state tfs = {0, 0, 0x7000};   // begin
    p_prim_restart pr = {1, 0xffff};
    p_rast_discard rd = {1};
    p_set_pool sp = {&pool_a};
    struct { uint32_t op; const void *pl; size_t len; const char *calls; } T[] = {
        {OP_BEGIN, NULL, 0, ""},
        {OP_SIGNAL_EVENT, NULL, 16, "signal_event"}, {OP_REPORT_COUNTER, NULL, 16, "report_counter"}, {OP_COPY_B2B, NULL, 24, "copy_b2b"}, {OP_UPDATE_UB, NULL, 24, "update_ub"},
        {OP_SET_RT, NULL, 16, "set_rt"}, {OP_CLEAR_COLOR, NULL, 24, "clear_color"}, {OP_CLEAR_DS, NULL, 16, "clear_ds"},
        {OP_VIEWPORTS, &vp, sizeof vp, "dirty"}, {OP_SCISSOR, &sc, sizeof sc, "dirty"}, {OP_DEPTH_RANGE, &dr, sizeof dr, "dirty"},
        {OP_BIND_PROGRAM, &bp, sizeof bp, "dirty bind_program"}, {OP_BIND_STATE, &bds, sizeof bds, "dirty"}, {OP_BIND_VATTR, &va, sizeof va, "dirty"},
        {OP_BIND_VSTREAM, &vs, sizeof vs, "dirty"}, {OP_BIND_VBUF, &vb, sizeof vb, "dirty"}, {OP_BIND_UBO, &ubo, sizeof ubo, "dirty"}, {OP_BIND_SSBO, &ssbo, sizeof ssbo, "dirty"},
        {OP_BIND_TEX, &bt, sizeof bt, "dirty"}, {OP_BIND_SAMP, &bsm, sizeof bsm, "dirty"}, {OP_STENCIL, &st, sizeof st, "dirty"}, {OP_POLY_OFFSET, &po, sizeof po, "dirty"},
        {OP_DRAW, NULL, 24, "draw"}, {OP_DRAW_IDX, NULL, 24, "draw_idx"}, {OP_DISPATCH, NULL, 32, "dispatch"}, {OP_BARRIER, NULL, 8, "barrier"},
        {OP_SET_TEX_POOL, &sp, sizeof sp, "dirty"}, {OP_SET_SAMP_POOL, &sp, sizeof sp, "dirty"}, {OP_COPY_B2T, NULL, 16, "copy_b2t"}, {OP_COPY_STRIDE, &cs, sizeof cs, "dirty"},
        {OP_BLEND_COLOR, &bc, sizeof bc, "dirty"}, {OP_COPY_T2B, NULL, 16, "copy_t2b"}, {OP_BIND_IMG, &bim, sizeof bim, "dirty"}, {OP_COPY_T2T, NULL, 16, "copy_t2t"},
        {OP_RESET_COUNTER, NULL, 8, "reset_counter"}, {OP_CLEAR_TEX, NULL, 16, "clear_tex"}, {OP_CLEAR_BUF, NULL, 24, "fill"}, {OP_DEPTH_CLAMP, &dc, sizeof dc, "dirty"},
        {OP_RENDER_COND, NULL, 16, "render_cond"}, {OP_FENCE_SYNC, NULL, 8, "fence_sync"}, {OP_TF_BIND, &tfb, sizeof tfb, "tf_bind dirty"},
        {OP_TF_STATE, &tfs, sizeof tfs, "tf_state_note dirty tf_state"}, {OP_RAST_DISCARD, &rd, sizeof rd, "rast_discard dirty"}, {OP_VP_SWIZZLE, &sw, sizeof sw, "dirty"},
        {OP_BIND_TEX1, &bt1, sizeof bt1, "dirty"}, {OP_PRIM_RESTART, &pr, sizeof pr, "dirty"}, {OP_BIND_UBO_SMALL, &ubs, sizeof ubs, "dirty"}, {OP_BIND_VBUF1, &vb1, sizeof vb1, "dirty"},
        {OP_COUNT_ + 3, NULL, 8, ""},   // an unknown opcode: only its pre
    };
    enum { NT = sizeof T / sizeof T[0] };
    const uint8_t *rec[NT]; uint8_t seen[OP_COUNT_] = {0}; int at[OP_COUNT_ + 8];
    for (int i = 0; i < NT; i++) {
        uint64_t z[4] = {0};
        rec[i] = emit(o, T[i].op, T[i].pl ? T[i].pl : (const void *)z, T[i].len);
        if (T[i].op < OP_COUNT_) seen[T[i].op] = 1;
        at[T[i].op] = i;
    }
    const uint8_t *o_end = emit0(o, OP_END);
    seen[OP_END] = 1; seen[OP_JUMP] = seen[OP_CALL] = 1;   // walked above
    int all = 1; for (int op = 0; op < OP_COUNT_; op++) all &= seen[op];
    t("the opcode list covers all 51 opcodes (with END, JUMP, CALL above)", all && OP_COUNT_ == 51);
    reset_ev();
    nvn_replay_exec((const uint8_t *)o->w);
    int bad = 0, i_ev = 1;   // ev[0] is list_begin
    char got[256];
    for (int i = 0; i < NT; i++) {
        if (strcmp(ev[i_ev].fn, "pre") || ev[i_ev].rec != rec[i]) { bad++; printf("  record %d: no pre\n", i); break; }
        int j = i_ev + 1; size_t g = 0; got[0] = 0;
        while (j < nev && strcmp(ev[j].fn, "pre")) {
            g += (size_t)snprintf(got + g, sizeof got - g, "%s%s", g ? " " : "", ev[j].fn);
            if (strcmp(ev[j].fn, "dirty") && (ev[j].rec != rec[i] || ev[j].op != T[i].op || !ev[j].a_ok)) { bad++; printf("  record %d: %s got another record\n", i, ev[j].fn); }
            if (!strcmp(ev[j].fn, "dirty") && !(ev[j].mask & NVN_STATE_APPLIED)) { bad++; printf("  record %d: a mask without NVN_STATE_APPLIED\n", i); }
            j++;
        }
        if (strcmp(got, T[i].calls)) { bad++; printf("  op %u: calls \"%s\", want \"%s\"\n", T[i].op, got, T[i].calls); }
        i_ev = j;
    }
    t("every opcode: its pre, then exactly its backend calls, in order", !bad);
    t("every opcode: the list ends at its END", ev[nev - 1].rec == o_end && !strcmp(ev[nev - 1].fn, "end"));
    // The state of the records went through nvn_state_apply
    t("state applied: scissor, viewport, depth range", R.has_sc && R.sc.w == 30 && S.has_vp && S.vp[0][2] == 1280 && S.dr[0] == 0.25f && S.dr[1] == 0.75f);
    t("state applied: program, depth-stencil, vertex input", S.prog == &P[0] && S.ds.dt == 1 && S.nattr == 1 && S.attr[0].fmt == 5 && S.nstream == 1 && S.vbuf[0].address == 0x1000 && S.vbuf[2].address == 0x2000);
    t("state applied: uniform, storage, texture, sampler, image binds", S.ubo[2][3].addr == 0x3000 && S.ubo[1][5].addr == 0x5000 && S.ssbo[4][1].addr == 0x4000 && S.tex[1][0] == 7 && S.tex[0][3] == 9
      && S.samp[1][0] == 3 && S.img[1][0] == 4 && n_hook_bind == 4);
    t("state applied: stencil, polygon offset, clamp, swizzle, blend colour, stride", S.st_vmask[0] == 0x0f && S.st_vmask[1] == 0x0f && S.po.units == 2 && S.depth_clamp == 1 && S.vp_sw[0][0] == 2
      && S.blend_color[0] == 0.5f && S.copy_stride == 256);
    t("state applied: pools, feedback, primitive restart, rasterizer discard", S.tex_base == 100 && S.samp_base == 200 && S.tf[0].addr == 0x6000 && S.tf_on == 1 && S.restart.on == 1 && S.restart.index == 0xffff
      && S.rast_discard == 1);
    // The masks: what nvn_state_apply returned
    t("mask: scissor applied, nothing dirty", mask_after(rec[at[OP_SCISSOR]]) == (int)NVN_STATE_APPLIED);
    t("mask: a new program dirties the pipeline and every stage's arguments", mask_after(rec[at[OP_BIND_PROGRAM]]) == (int)(NVN_STATE_APPLIED | NVN_DIRTY_PIPELINE | NVN_DIRTY_ARGS_ALL));
    t("mask: depth-stencil state", mask_after(rec[at[OP_BIND_STATE]]) == (int)(NVN_STATE_APPLIED | NVN_DIRTY_DEPTH_STENCIL));
    t("mask: a uniform bind dirties its stage (2)", mask_after(rec[at[OP_BIND_UBO]]) == (int)(NVN_STATE_APPLIED | NVN_DIRTY_ARGS(2)));
    t("mask: a texture pool with a new base dirties every stage's arguments", mask_after(rec[at[OP_SET_TEX_POOL]]) == (int)(NVN_STATE_APPLIED | NVN_DIRTY_ARGS_ALL));
    t("mask: rasterizer discard dirties the pipeline", mask_after(rec[at[OP_RAST_DISCARD]]) == (int)(NVN_STATE_APPLIED | NVN_DIRTY_PIPELINE));
    t("mask: stencil masks dirty the depth-stencil state", mask_after(rec[at[OP_STENCIL]]) == (int)(NVN_STATE_APPLIED | NVN_DIRTY_DEPTH_STENCIL));
    t("mask: transform feedback state, nothing dirty", mask_after(rec[at[OP_TF_STATE]]) == (int)NVN_STATE_APPLIED && mask_after(rec[at[OP_TF_BIND]]) == (int)NVN_STATE_APPLIED);

    // ---------- Where the state is applied relative to the backend's work (the Metal arm's order) ----------
    int ib = find("bind_program", 0), itb = find("tf_bind", 0), itn = find("tf_state_note", 0), its = find("tf_state", 0), ird = find("rast_discard", 0);
    t("BIND_PROGRAM: applied first; be_bind_program sees the new program", ib > 0 && !strcmp(ev[ib - 1].fn, "dirty") && ev[ib].prog == &P[0] && ev[ib - 2].prog == NULL);
    t("TF_BIND: be_tf_bind first (buffer 0 not yet bound), then the apply", itb > 0 && !strcmp(ev[itb + 1].fn, "dirty") && ev[itb].tf0.addr == 0 && ev[itb + 2].tf0.addr == 0x6000);
    t("TF_STATE: the note before the apply (feedback off)", itn > 0 && !strcmp(ev[itn + 1].fn, "dirty") && ev[itn].tf_on == 0);
    t("TF_STATE: be_tf_state after the apply (feedback on)", its == itn + 2 && ev[its].tf_on == 1 && ev[its].rec == ev[itn].rec);
    t("RAST_DISCARD: be_rast_discard before the apply (discard off), then the mask", ird > 0 && ev[ird].rast_discard == 0 && !strcmp(ev[ird + 1].fn, "dirty") && ev[ird + 2].rast_discard == 1);
    int iu = find_pre(rec[at[OP_COUNT_ + 3]]), ig = find_pre(rec[at[OP_BEGIN]]);
    t("an unknown opcode and OP_BEGIN: their pre only, the walk goes on", iu > 0 && ev[iu + 1].rec == o_end && !strcmp(ev[iu + 1].fn, "pre") && ig == 1 && ev[ig + 1].rec == rec[1]);

    printf("== t_replay: %d checks, %d failed ==\n", total, fail);
    return fail != 0;
}
