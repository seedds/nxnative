// The look-ahead over the rest of a submission (docs/design/vulkan-backend.md 3.3; phase P5): the Vulkan backend's counterpart of the Metal backend's pso_predict_rest_ (nvn_mtl.m).
//
// When. After a draw waited for its pipeline (made it on the submit thread, or waited for a worker's build: hvk_pso_take_waited), once per submission (the open submission's
// fence, as the Metal backend marks its submission serial in g_pp_submit_mark), and after that draw's own pipeline is made: demand first.
//
// What it walks. The records still to come, in replay order, from the record after the draw: through OP_JUMP within a list, into OP_CALLed sub-lists (with a return stack of its
// own), back into the callers whose continuations the frontend's decoder keeps (nvn_replay_ret, nvn_replay.h), and on into the command handles of the submission still to be
// replayed (be_exec_rest, hvk_predict_rest_handles). The Metal walk (fz_next from g_exec_next with an empty return stack) goes from the end of a called list to the next handle;
// this one returns into the callers first, as the replay does.
//
// Over what. A private copy of the state a pipeline key depends on, taken from the frontend's S and R at the draw (read, never written) and changed by the walked records as
// nvn_state_apply changes S: the program bound per stage (OP_BIND_PROGRAM: the shaders of a stage come from the program bound there, nvn_state_bind_stages's rule, without its
// composite-program hook), the blend, colour, channel mask, multisample and polygon state blocks (OP_BIND_STATE; depth-stencil state is dynamic in every level), the vertex
// attributes and streams and which streams have a buffer (OP_BIND_VATTR, OP_BIND_VSTREAM, OP_BIND_VBUF, OP_BIND_VBUF1), the depth clamp (OP_DEPTH_CLAMP), the render targets
// (OP_SET_RT) and each draw's primitive class (OP_DRAW, OP_DRAW_IDX). Every input of a key is in those records and their payloads (nvn_obj.h): none needs the frontend's decoder.
// The Metal walk instead applies the records to the live S and copies it back after (state_op, then memcpy and prog_resolve), which needs the frontend's state save and its ring
// of composite programs; this one changes nothing the replay reads, so the frontend's records, nvn_be.h and the Metal backend stay as they are.
//
// The key. Made as the draw makes it (vk_commands.c hvk_draw: hvk_vertex_input, hvk_key_fill, hvk_layout_for), with the attachments keyed as a prewarm row's are (hvk_attach_format
// of the view's NVN format, else the texture's: the rule that lets a row name its draw's pipeline, vk_prewarm.c), since the walk makes no texture or view. Where a texture held in
// a substitute is attached through a view of another format, the image's format can differ from that rule's: that key then names another pipeline, which is built for nothing,
// and the draw makes its own, as without the look-ahead. A key is made only when its state changed since the last draw walked.
//
// What it does with a key: hvk_pso_predict (vk_gpl.c) queues it on the background workers, in draw order, unless it is ready, being made or queued, 32 keys at a time with one
// lock of the pipeline table and one wake of the workers (a lock and a wake a key took most of a walk's time, and much more on a loaded CPU). The draw that comes for it finds
// it made, waits for the rest of a build under way, or, when no worker has taken it, makes it itself (no draw waits behind the queue). What is drawn never changes: a draw binds the
// pipeline of its own key, made by whichever thread.
//
// Bounds: at most 200,000 records (the Metal walk's guard) and 2 ms (the wait budget of the P5 acceptance) a walk, then it stops, counted and logged (the first 8 times); a call
// deeper than its return stack stops it too. DM_NVN_PREDICT=0 turns it off, as it turns off the Metal backend's.
#include "plat/plat.h"
#include "vk_pso.h"
#include "../nvn/nvn_state.h"
#include "../nvn/nvn_replay.h"
#include <stdlib.h>
#include <string.h>

enum { MAX_RECORDS = 200000, NRET = 2 * NVN_REPLAY_RET_MAX };
#define MAX_NS 2000000ull
static const uint64_t *g_rest; static int g_nrest;   // the submission's handles still to be replayed after the current one (be_exec_rest)
static uint64_t g_mark;                                 // the submission walked last (its fence): one walk a submission
static _Atomic int g_on = -1; static int g_sync; static uint32_t g_max_records = MAX_RECORDS; static uint64_t g_max_ns = MAX_NS;
static _Atomic uint64_t s_walks, s_records, s_ns, s_max_ns, s_budget, s_depth, s_draws, s_keys, s_queued;

static int on(void) {
    int v = atomic_load(&g_on);
    if (v >= 0) return v;
    const char *e = getenv("DM_NVN_PREDICT"); v = !(e && !strcmp(e, "0"));
    if (!v) hostvk_log("[hostvk] DM_NVN_PREDICT=0: no look-ahead of pipelines over the rest of a submission");
    atomic_store(&g_on, v);
    return v;
}
void hvk_predict_rest_handles(const uint64_t *h, int n) { g_rest = h; g_nrest = h ? n : 0; }
void hvk_predict_test(int on_, int sync, uint32_t max_records, uint64_t max_ns) {
    atomic_store(&g_on, on_ != 0); g_sync = sync; g_max_records = max_records ? max_records : MAX_RECORDS; g_max_ns = max_ns ? max_ns : MAX_NS;
}
void hvk_predict_stats_get(hvk_predict_stats *s) {
    memset(s, 0, sizeof *s);
    s->walks = s_walks; s->records = s_records; s->ns = s_ns; s->max_ns = s_max_ns; s->budget_stops = s_budget; s->depth_stops = s_depth;
    s->draws = s_draws; s->keys = s_keys; s->queued = s_queued; s->on = on();
}

// ---------- The private state ----------
typedef struct {
    const prog_t *bst[5];                // the program bound per graphics stage (S.bst)
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly;
    va_st attr[16]; vs_st stream[16]; uint64_t vbuf[16];   // vbuf: each stream's buffer address (0: none)
    int depth_clamp;
    hvk_key_src rt;                      // the attachments' share of the key: formats, classes, samples
} wstate;
static int is_depth(VkFormat f) { return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }
static int is_stencil(VkFormat f) { return f == VK_FORMAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }
static int attachable(const tex_t *t) { return t && t->serial && t->d.target != NVN_TEXTURE_TARGET_BUFFER; }
static uint32_t view_level(const view_t *v) { return v && v->set_levels ? (uint32_t)v->base_level : 0; }
static uint32_t view_layer(const view_t *v) { return v && v->set_layers ? (uint32_t)v->min_layer : 0; }
// The attachments of targets rt as vk_commands.c pass_begin attaches them: a slot whose texture is already attached at the same level and layer is left out (the Metal backend's
// rt_dup_slot), trailing slots with nothing attached are not the pass's, the depth texture's aspects give the depth and stencil formats, the samples are the last attached one's
static void rt_src(const p_set_rt *rt, int has_rt, hvk_key_src *s) {
    memset(s, 0, sizeof *s);
    int samples = 0, nc = has_rt ? rt->n : 0; if (nc > 8) nc = 8;
    for (int i = 0; i < nc; i++) {
        const tex_t *t = rt->color[i];
        if (!attachable(t)) continue;
        const view_t *v = rt->has_cview[i] ? &rt->cview[i] : NULL;
        int dup = 0;
        for (int j = 0; j < i && !dup; j++) if (s->cfmt[j] && rt->color[j]->serial == t->serial) {
            const view_t *u = rt->has_cview[j] ? &rt->cview[j] : NULL;
            dup = view_level(u) == view_level(v) && view_layer(u) == view_layer(v);
        }
        if (dup) continue;
        uint8_t cls = 0; VkFormat f = hvk_attach_format(v && v->set_format ? (int)v->format : (int)t->d.format, &cls);
        if (!f) continue;
        s->cfmt[i] = f; s->ccls[i] = cls; s->nc = i + 1; samples = t->d.samples > 1 ? t->d.samples : 1;
    }
    if (has_rt && attachable(rt->depth)) {
        const view_t *v = rt->has_dview ? &rt->dview : NULL;
        VkFormat img = hvk_attach_format((int)rt->depth->d.format, NULL), f = v && v->set_format ? hvk_attach_format((int)v->format, NULL) : img;
        if (img && f) { if (is_depth(img)) s->dfmt = f; if (is_stencil(img)) s->sfmt = f; samples = rt->depth->d.samples > 1 ? rt->depth->d.samples : 1; }
    }
    s->samples = samples ? (VkSampleCountFlagBits)samples : VK_SAMPLE_COUNT_1_BIT;
}
// The shaders a draw would take from the stages' programs: the program itself where every bound stage has it and the fragment stage does (S.prog is that program, every one of
// its stages taken), else each stage from the program bound there (a composite, hvk_prog_get's rule). 0 when the draw has no program (it is skipped)
static int shaders_of(const prog_t *const bst[5], hvk_shader *sh[6]) {
    memset(sh, 0, 6 * sizeof *sh);
    const prog_t *v = bst[0];
    if (!v) return 0;
    int same = 1; for (int st = 1; st < 5; st++) if (bst[st] && bst[st] != v) same = 0;
    if (same && bst[1] == v) {
        hvk_prog *p = v->serial ? hvk_prog_get(v) : NULL;
        if (!p) return 0;
        for (int st = 0; st < 5; st++) sh[st] = p->sh[st];
        return 1;
    }
    for (int st = 0; st < 5; st++) if (bst[st]) { hvk_prog *p = bst[st]->serial ? hvk_prog_get(bst[st]) : NULL; if (!p) return 0; sh[st] = p->sh[st]; }
    return 1;
}
static int prim_class(int prim) {   // the topology class of an NVN primitive, as vk_commands.c topology() gives it
    switch (prim) {
        case NVN_DRAW_PRIMITIVE_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case NVN_DRAW_PRIMITIVE_LINES: case NVN_DRAW_PRIMITIVE_LINE_LOOP: case NVN_DRAW_PRIMITIVE_LINE_STRIP: case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY:
        case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case NVN_DRAW_PRIMITIVE_PATCHES: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}
// The keys made since the last flush, queued together (hvk_pso_predict: one lock and one wake of the workers for the batch); submit thread
enum { BATCH = 32 };
static hvk_pred_key g_batch[BATCH]; static int g_nbatch;
static uint64_t flush(void) { int n = g_nbatch; g_nbatch = 0; return n ? (uint64_t)hvk_pso_predict(g_batch, n) : 0; }
// A draw walked: its key, added to the batch (flushed when full: the pipelines queued then)
static uint64_t draw_key(const wstate *w, int klass) {
    hvk_shader *sh[6];
    if (!shaders_of(w->bst, sh) || !sh[0] || (sh[4] && !sh[3])) return 0;   // no program, no vertex shader, tessellation without a control shader: the draw is skipped
    hvk_layout *l = hvk_layout_for(sh);
    if (!l) return 0;
    uint32_t bound = 0; for (int s = 0; s < 16; s++) if (w->vbuf[s]) bound |= 1u << s;
    hvk_vin v; hvk_vertex_input(sh[0], w->attr, w->stream, bound, &v);
    hvk_key_src src = w->rt;
    src.blend = w->blend; src.blend_en = w->color.blend_en; src.chmask = w->chmask.mask; src.logic_op = w->color.logic_op; src.poly_mode = w->poly.mode;
    src.depth_clamp = w->depth_clamp; src.a2c = w->ms.a2c; src.vin = &v;
    hvk_pred_key *p = &g_batch[g_nbatch++];
    memcpy(p->sh, sh, sizeof p->sh); p->l = l; hvk_key_fill(&p->k, sh, klass, &src);
    atomic_fetch_add(&s_keys, 1);
    return g_nbatch == BATCH ? flush() : 0;
}
#define SET(dst, src, n) do { if (memcmp(&(dst), (src), (n))) { memcpy(&(dst), (src), (n)); dirty = 1; } } while (0)
static void walk(const uint8_t *p) {
    uint64_t t0 = plat_now_ns();
    wstate w; memset(&w, 0, sizeof w);
    memcpy(w.bst, S.bst, sizeof w.bst); memcpy(w.blend, S.blend, sizeof w.blend); w.color = S.color; w.chmask = S.chmask; w.ms = S.ms; w.poly = S.poly;
    memcpy(w.attr, S.attr, sizeof w.attr); memcpy(w.stream, S.stream, sizeof w.stream); for (int s = 0; s < 16; s++) w.vbuf[s] = S.vbuf[s].address;
    w.depth_clamp = S.depth_clamp;
    rt_src(&R.rt, R.has_rt, &w.rt);
    // the cursor: the callers' continuations the decoder keeps, the innermost last; past NVN_REPLAY_RET_MAX the decoder keeps none, and the walk stops where it would need them
    const uint8_t *ret[NRET]; int ret_n = nvn_replay_ret_n < NVN_REPLAY_RET_MAX ? nvn_replay_ret_n : NVN_REPLAY_RET_MAX, lost = nvn_replay_ret_n > NVN_REPLAY_RET_MAX, base = ret_n;
    memcpy(ret, nvn_replay_ret, (size_t)ret_n * sizeof *ret);
    int rest_i = 0, dirty = 1, last_klass = -1, stop = 0;
    uint64_t n = 0, draws = 0, queued = 0;
    while (p) {
        if (n >= g_max_records || (!(n & 63) && plat_now_ns() - t0 > g_max_ns)) { stop = 1; break; }
        op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        n++;
        if (h.op == OP_END) {
            if (ret_n > base || (ret_n && !lost)) { p = ret[--ret_n]; continue; }
            if (lost) { stop = 2; break; }
            p = rest_i < g_nrest ? (const uint8_t *)(uintptr_t)g_rest[rest_i++] : NULL;   // the next handle of the submission, or its end
            continue;
        }
        if (h.op == OP_JUMP) { memcpy(&p, a, sizeof p); continue; }
        if (h.op == OP_CALL) {
            if (ret_n >= NRET) { stop = 2; break; }
            ret[ret_n++] = p + h.size; memcpy(&p, a, sizeof p); continue;
        }
        if (h.size < 8) break;   // never written by the recorder: nothing to step over it with
        switch (h.op) {
            case OP_BIND_PROGRAM: { const p_bind_program *b = a;
                for (int st = 0; st < 5; st++) if ((b->stages >> st & 1) && w.bst[st] != b->prog) { w.bst[st] = b->prog; dirty = 1; }
                break; }
            case OP_BIND_STATE: { const p_bind_state *b = a;
                switch (b->kind) {
                    case 0: { blend_st bs; memcpy(&bs, b->bytes, sizeof bs); SET(w.blend[bs.target], &bs, sizeof bs); break; }
                    case 1: SET(w.color, b->bytes, sizeof w.color); break;
                    case 2: SET(w.chmask, b->bytes, sizeof w.chmask); break;
                    case 3: SET(w.ms, b->bytes, sizeof w.ms); break;
                    case 4: SET(w.poly, b->bytes, sizeof w.poly); break;
                    default: break;   // depth-stencil: dynamic on every level
                }
                break; }
            case OP_BIND_VATTR: { const p_bind_vattr *b = a; int k = b->n < 0 ? 0 : b->n > 16 ? 16 : b->n; SET(w.attr, b->st, 4 * (size_t)k); break; }
            case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; int k = b->n < 0 ? 0 : b->n > 16 ? 16 : b->n; SET(w.stream, b->st, 8 * (size_t)k); break; }
            case OP_BIND_VBUF: case OP_BIND_VBUF1: { p_bind_vbuf t; const p_bind_vbuf *b = vbuf_rec(h.op, a, &t);
                for (int i = 0; i < b->n && i < 16; i++) { int s = b->first + i; if (s < 0 || s >= 16) continue;
                    uint64_t ad = b->r[i].address; if (!ad != !w.vbuf[s]) dirty = 1; w.vbuf[s] = ad; }   // only whether a stream has a buffer is in the key
                break; }
            case OP_DEPTH_CLAMP: { int c = ((const p_depth_clamp *)a)->on; if (c != w.depth_clamp) { w.depth_clamp = c; dirty = 1; } break; }
            case OP_SET_RT: rt_src(a, 1, &w.rt); dirty = 1; break;
            case OP_DRAW: case OP_DRAW_IDX: {
                int prim; if (h.op == OP_DRAW) { p_draw t; prim = draw_rec(a, h.size, &t)->prim; } else { p_draw_idx t; prim = draw_idx_rec(a, h.size, &t)->prim; }
                int klass = prim_class(prim);
                draws++;
                if (dirty || klass != last_klass) { dirty = 0; last_klass = klass; queued += draw_key(&w, klass); }
                break; }
            default: break;
        }
        p += h.size;
    }
    queued += flush();
    uint64_t dt = plat_now_ns() - t0, o = atomic_load(&s_max_ns);
    while (dt > o && !atomic_compare_exchange_weak(&s_max_ns, &o, dt)) {}
    atomic_fetch_add(&s_walks, 1); atomic_fetch_add(&s_records, n); atomic_fetch_add(&s_ns, dt); atomic_fetch_add(&s_draws, draws); atomic_fetch_add(&s_queued, queued);
    if (stop == 1) {
        static int logged; atomic_fetch_add(&s_budget, 1);
        if (logged++ < 8) hostvk_log("[hostvk] pipeline prediction: the walk of the rest of the submission stopped at its budget (%llu records in %.3f ms; the budget %u records, %.1f ms) after %llu draws, "
                                     "%llu pipelines queued; the draws past it make their own", (unsigned long long)n, dt / 1e6, g_max_records, g_max_ns / 1e6, (unsigned long long)draws, (unsigned long long)queued);
    } else if (stop == 2) {
        atomic_fetch_add(&s_depth, 1);
        HVK_NOTE_ONCE("pipeline prediction: a walk stopped at a list nested deeper than its return stack holds (%d lists, the decoder's %d): the draws past it make their own", NRET, NVN_REPLAY_RET_MAX);
    }
    static int told; if (queued && stop != 1 && told++ < 8) hostvk_log("[hostvk] pipeline prediction: %llu draws ahead in this submission, %llu pipelines queued on the background workers (%llu records walked in %.3f ms)",
                                                                       (unsigned long long)draws, (unsigned long long)queued, (unsigned long long)n, dt / 1e6);
    if (g_sync) hvk_work_drain(HVK_Q_PRED);   // test hook: the draws ahead find their pipelines made
}
void hvk_predict_after_draw(const uint8_t *next) {
    if (!hvk_pso_take_waited() || !next || !on()) return;
    uint64_t sub = hvk_open_fence();
    if (!sub || sub == g_mark) return;
    g_mark = sub;
    walk(next);
}
