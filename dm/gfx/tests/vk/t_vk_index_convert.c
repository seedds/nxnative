// The backend's index conversions (dm/gfx/hostvk/vk_index.c, its rules table; docs/design/vulkan-backend.md 2.7): every NVN primitive the test can rasterize without a geometry or
// tessellation stage (points, lines, line loops, line strips, triangles, triangle strips, triangle fans, quads, quad strips, polygons, and the line and triangle lists with
// adjacency), with 1-, 2- and 4-byte indices and without indices, primitive restart off, at the all-ones index and at a custom one, is drawn through the backend and compared
// pixel for pixel with a reference: the same primitives drawn as a plain point, line or triangle list of 32-bit indices that the test builds on the CPU with its own converter (the
// oracle below, written from the rules, not from the backend's code). Two targets show a difference: the first adds 1/255 per primitive over a pixel, front faces in red and back
// faces in green (a primitive missing, doubled or wound the other way changes it), the second keeps the ID of the last primitive drawn over the pixel (a primitive out of order, or
// a vertex in another primitive, changes it). The streams have a restart index first, two in a row, segments of 1 to 11 vertices (incomplete primitives within and at the end),
// a short one and a long one (1,040 indices: the restart kernel's 128 threads take chunks of 9) ending with a restart index, drawn with 1 and 2 instances. Then: fans and polygons
// with the device's triangle fans taken as absent, and 8-bit indices with its 8-bit indices taken as absent (hvk_test_index_opts: the conversions those devices need); lists
// with restart both ways, natively where the device restarts lists (VK_EXT_primitive_topology_list_restart: lavapipe does) and converted with that taken as absent; indices a
// GPU copy writes earlier in the submission (converted at the draw's place, not in the prologue); 0 to 5 indices and a stream of restart indices only; the GPU path's limit
// lowered to 2,400 bytes, a list of exactly that many and one past it (converted on the CPU, counted), then every short stream with the limit at 4 bytes (the CPU's list must be
// the GPU's); 400 conversions of six forms in one submission, three submissions in a row (the ring's growth and reuse); and the conversion counters (where each ran).
// Draws whose count the GPU holds (section 7, 2026-11-02; vk_index.c hvk_ix_ind_gpu): the whole matrix again through an indirect record (first index 3, the base vertex,
// instances and a base instance in the record) that the CPU writes before the submission (converted in the prologue) and that a dispatch writes earlier in the submission
// (shaders/ixc_copy.comp; the region holds 0xEE bytes when the draw is encoded, so a conversion that reads it too early draws garbage), the native forms through
// vkCmdDraw*Indirect; fans, 8-bit indices and list restart taken as absent; indirect-count multi-draws of five records with the count 3, 0 and 9; a count and an instance count
// of 0; the cuts on the GPU, counted (the arena lowered to 600 indices: filled by one draw, the next cut to nothing, one draw cut to what is left; a draw's input lowered to 100
// indices; 65,537 quads without indices over the shared list of 65,536, under rasterizer discard; a record past its index buffer's pool); DrawTransformFeedback of 69 captured
// points as every primitive, the capture in the same submission and in the one before, and fans and polygons with the device's fans taken as absent (only buffer 0 is bound and
// captured: lavapipe crashes on a counter buffer for an unbound buffer, the README's P8 finding). Checks: one per draw and the counters, and the validation check.
// With the argument "bench" (not a CTest run; validation off): the submit thread's time per converted draw on the CPU path and on the GPU path, for a 10,000-quad draw, a
// 100,000-index fan with restart and a 6-quad draw, three runs.
#include "t_vk_harness.h"
#include "shaders/gen/ixc_vert.h"
#include "shaders/gen/ixc_frag.h"
#include "shaders/gen/ixc_copy_comp.h"
#include "shaders/gen/xfb5_vert.h"
#include <time.h>

void hvk_test_index_opts(int no_fans, int no_uint8, int no_list_restart, int64_t limit);   // vk_index.c: -1 keeps an option; limit 0 is the default
void hvk_test_index_ind(int64_t arena_bytes, int64_t in_max);                              // the arena's bytes, the indices a draw whose count the GPU holds reads (0 the defaults)
void hvk_test_index_stats(uint64_t s[21]);
enum { ST_GPU, ST_GPU_PRE, ST_GPU_RESTART, ST_GPU_IN, ST_GPU_OUT, ST_GPU_NS, ST_CPU, ST_CPU_OVER, ST_CPU_NOADDR, ST_CPU_IN, ST_CPU_OUT, ST_CPU_NS,
       ST_IND, ST_IND_PRE, ST_IND_REC, ST_IND_XFB, ST_IND_MDI, ST_IND_NS, ST_CUT_ARENA, ST_CUT_LIMIT, ST_CUT_POOL, ST_N };

enum { W = 64, H = 64, MAXN = 120000 };
static NVNqueue g_q; static hv_pool g_mp; static hv_cmds g_c; static NVNprogram g_prog;
static uint8_t *g_ib, *g_stage, *g_ref;              // index streams (one region per draw of a submission), a copy's source, the references' lists
static size_t g_ib_used, g_ref_used, g_stage_used;   // g_stage_used: the staging of the dispatches that write what indirect draws read (gpu_write)
static tex_t g_t[4];                                 // the draw's two targets, the reference's two
static uint32_t g_v[MAXN], g_list[3 * MAXN];
static unsigned g_seed = 12345;
static unsigned rnd(void) { g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 8; }
static const char *const k_name[] = {"points", "lines", "line loop", "line strip", "triangles", "triangle strip", "triangle fan", "quads", "quad strip", "polygon",
                                     "lines adjacency", "line strip adjacency", "triangles adjacency"};
static const int k_prims[] = {NVN_DRAW_PRIMITIVE_POINTS, NVN_DRAW_PRIMITIVE_LINES, NVN_DRAW_PRIMITIVE_LINE_LOOP, NVN_DRAW_PRIMITIVE_LINE_STRIP, NVN_DRAW_PRIMITIVE_TRIANGLES,
                              NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, NVN_DRAW_PRIMITIVE_TRIANGLE_FAN, NVN_DRAW_PRIMITIVE_QUADS, NVN_DRAW_PRIMITIVE_QUAD_STRIP, NVN_DRAW_PRIMITIVE_POLYGON,
                              NVN_DRAW_PRIMITIVE_LINES_ADJACENCY, NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY};
enum { NPRIM = sizeof k_prims / sizeof *k_prims };

// ---------- The oracle ----------
// The primitives NVN draws (the rules: a restart index, indexed draws only, compared before the base vertex is added, ends a segment and drops the vertices of a primitive it cuts
// short; a line loop closes from its last vertex to its first; a fan or polygon is (0, k+1, k+2); quad k is (4k, 4k+1, 4k+2) (4k, 4k+2, 4k+3); a quad strip's quad k is
// (2k, 2k+1, 2k+3, 2k+2), split as quads; without a geometry stage a line list with adjacency draws (4k+1, 4k+2) and a triangle list with adjacency (6k, 6k+2, 6k+4)), as the
// absolute vertex indices of a point (cls 1), line (2) or triangle (3) list. v: the stream (NULL: none, vertex k is base + k)
static int cls_of(int prim) {
    switch (prim) {
        case NVN_DRAW_PRIMITIVE_POINTS: return 1;
        case NVN_DRAW_PRIMITIVE_LINES: case NVN_DRAW_PRIMITIVE_LINE_LOOP: case NVN_DRAW_PRIMITIVE_LINE_STRIP: case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: return 2;
        default: return 3;
    }
}
static int is_list(int prim) {   // the lists the test draws: points, lines, triangles and the two with adjacency
    return prim == NVN_DRAW_PRIMITIVE_POINTS || prim == NVN_DRAW_PRIMITIVE_LINES || prim == NVN_DRAW_PRIMITIVE_TRIANGLES || prim == NVN_DRAW_PRIMITIVE_LINES_ADJACENCY ||
           prim == NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY;
}
static uint32_t oracle(int prim, const uint32_t *v, uint32_t n, int restart, uint32_t ri, int64_t base, uint32_t *out) {
    uint32_t o = 0;
    for (uint32_t st = 0, i = 0; i <= n; i++) {
        if (i < n && !(v && restart && v[i] == ri)) continue;
        uint32_t m = i - st;
#define X(k) ((uint32_t)((v ? (int64_t)v[st + (k)] : (int64_t)(st + (k))) + base))
#define T(a, b, c) do { out[o++] = X(a); out[o++] = X(b); out[o++] = X(c); } while (0)
#define L(a, b) do { out[o++] = X(a); out[o++] = X(b); } while (0)
        switch (prim) {
            case NVN_DRAW_PRIMITIVE_POINTS: for (uint32_t k = 0; k < m; k++) out[o++] = X(k); break;
            case NVN_DRAW_PRIMITIVE_LINES: for (uint32_t k = 0; k + 2 <= m; k += 2) L(k, k + 1); break;
            case NVN_DRAW_PRIMITIVE_LINE_LOOP: for (uint32_t k = 0; m >= 2 && k < m; k++) L(k, (k + 1) % m); break;
            case NVN_DRAW_PRIMITIVE_LINE_STRIP: for (uint32_t k = 0; k + 1 < m; k++) L(k, k + 1); break;
            case NVN_DRAW_PRIMITIVE_TRIANGLES: for (uint32_t k = 0; k + 3 <= m; k += 3) T(k, k + 1, k + 2); break;
            case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: for (uint32_t k = 0; k + 2 < m; k++) { if (k & 1) T(k + 1, k, k + 2); else T(k, k + 1, k + 2); } break;
            case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: case NVN_DRAW_PRIMITIVE_POLYGON: for (uint32_t k = 0; k + 2 < m; k++) T(0, k + 1, k + 2); break;
            case NVN_DRAW_PRIMITIVE_QUADS: for (uint32_t k = 0; k + 4 <= m; k += 4) { T(k, k + 1, k + 2); T(k, k + 2, k + 3); } break;
            case NVN_DRAW_PRIMITIVE_QUAD_STRIP: for (uint32_t k = 0; k + 4 <= m; k += 2) { T(k, k + 1, k + 3); T(k, k + 3, k + 2); } break;
            case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: for (uint32_t k = 0; k + 4 <= m; k += 4) L(k + 1, k + 2); break;
            case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: for (uint32_t k = 0; k + 6 <= m; k += 6) T(k, k + 2, k + 4); break;
            default: break;
        }
#undef X
#undef T
#undef L
        st = i + 1;
    }
    return o;
}

// ---------- Streams ----------
// A random walk over the 256 cells of the vertex shader's grid (neighbouring indices are neighbouring cells, so primitives stay small), segments of the lengths below between
// restart values rv: one first, two in a row, then 1 to 11 vertices; the long form repeats them to 1,040 indices and ends with rv. Values never equal skip (the custom restart
// index, so that only rv restarts), and stay below 256 for 1-byte indices
static const int k_segs[] = {0, 5, 0, 1, 2, 3, 4, 6, 7, 9, 11, 10};
static uint32_t stream(uint32_t *v, int longer, uint32_t rv, uint32_t skip) {
    uint32_t n = 0, w = rnd() % 200 + 20;
    for (int rep = 0; rep < (longer ? 15 : 1); rep++)
        for (unsigned s = 0; s < sizeof k_segs / sizeof *k_segs; s++) {
            if (s || rep) v[n++] = rv;
            for (int k = 0; k < k_segs[s]; k++) {
                w = (w + 256 + (rnd() % 35) - 17) % 240 + 8;
                if (w == skip) w++;
                v[n++] = w;
            }
        }
    if (longer) v[n++] = rv;
    return n;
}
static uint8_t *put_stream(uint8_t *dst, const uint32_t *v, uint32_t n, int isz) {
    for (uint32_t i = 0; i < n; i++) memcpy(dst + (size_t)i * (size_t)isz, &v[i], (size_t)isz);   // little-endian: the low bytes
    return dst;
}
static uint8_t *ib_take(size_t bytes) {   // a region of its own for each draw of a submission (16-byte aligned)
    uint8_t *p = g_ib + g_ib_used; g_ib_used += (bytes + 15) & ~(size_t)15;
    if (g_ib_used > (4u << 20)) { fprintf(stderr, "index regions full\n"); exit(2); }
    return p;
}
static uint8_t *ref_take(size_t bytes) {
    uint8_t *p = g_ref + g_ref_used; g_ref_used += (bytes + 15) & ~(size_t)15;
    if (g_ref_used > (6u << 20)) { fprintf(stderr, "reference regions full\n"); exit(2); }
    return p;
}

// ---------- Records ----------
static void targets(int which) {   // the draw's targets (0) or the reference's (1), cleared, with the test's state
    const tex_t *rt[2] = {&g_t[2 * which], &g_t[2 * which + 1]};
    hv_rt(&g_c, 2, rt, NULL);
    hv_clear_color(&g_c, 0, 15, 0, 0, 0, 0); hv_clear_color(&g_c, 1, 15, 0, 0, 0, 0);
    hv_bind(&g_c, &g_prog, 3); hv_depth(&g_c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&g_c, NVN_FACE_NONE);
    blend_st b; memset(&b, 0, sizeof b); b.target = 0; b.src_rgb = b.dst_rgb = b.src_a = b.dst_a = NVN_BLEND_FUNC_ONE; b.eq_rgb = b.eq_a = NVN_BLEND_EQUATION_ADD; hv_state(&g_c, 0, &b, sizeof b);
    color_st cs; memset(&cs, 0, sizeof cs); cs.blend_en = 1; cs.logic_op = NVN_LOGIC_OP_COPY; hv_state(&g_c, 1, &cs, sizeof cs);
    chmask_st m = {0xffffffffu}; hv_state(&g_c, 2, &m, sizeof m);
}
static void restart_rec(int on, uint32_t ri) { p_prim_restart pr = {(uint32_t)on, ri}; hv_emit(&g_c, OP_PRIM_RESTART, &pr, sizeof pr); }
// A draw as the game records it: indexed (isz 1, 2, 4: from a region of its own; gpu_written: copied there by the GPU earlier in the submission, the region holding 0xEE
// bytes when the draw is encoded) or not (isz 0: first = base); and its reference: the oracle's list of 32-bit indices as a plain list, restart off. Returns the primitives
static uint32_t draw_pair(int prim, int isz, int restart, uint32_t ri, const uint32_t *v, uint32_t n, int32_t base, uint32_t inst, int gpu_written, int which_draw, int which_ref) {
    uint32_t k = oracle(prim, isz ? v : NULL, n, isz && restart, ri, base, g_list), cls = (uint32_t)cls_of(prim);
    if (which_draw >= 0) {
        if (which_draw < 2) targets(which_draw);
        restart_rec(restart, ri);
        if (isz) {
            uint8_t *ib = ib_take((size_t)n * (size_t)isz + 4);
            if (gpu_written) {
                put_stream(g_stage, v, n, isz); memset(ib, 0xEE, (size_t)n * (size_t)isz);
                hv_b2b(&g_c, g_stage, ib, (uint64_t)n * (uint64_t)isz);
            } else put_stream(ib, v, n, isz);
            hv_draw_idx(&g_c, prim, isz == 1 ? 0 : isz == 2 ? 1 : 2, (int)n, ib, base, (int)inst);
        } else hv_draw(&g_c, prim, base, (int)n, (int)inst);
    }
    if (which_ref >= 0) {
        if (which_ref < 2) targets(which_ref);
        restart_rec(0, 0);
        uint8_t *rb = ref_take((size_t)k * 4 + 4); memcpy(rb, g_list, (size_t)k * 4);
        hv_draw_idx(&g_c, cls == 1 ? NVN_DRAW_PRIMITIVE_POINTS : cls == 2 ? NVN_DRAW_PRIMITIVE_LINES : NVN_DRAW_PRIMITIVE_TRIANGLES, 2, (int)k, rb, 0, (int)inst);
    }
    return k / cls;
}
// The draw's pictures against the reference's: 1 when both targets are equal pixel for pixel (and the reference drew something when the oracle has more than two primitives)
static uint8_t g_px[4][W * H * 4];
static int compare(uint32_t prims, const char *name) {
    for (int i = 0; i < 4; i++) if (!hv_read(&g_t[i], VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, W, H, 4, g_px[i])) { fprintf(stderr, "%s: readback %d failed\n", name, i); return 0; }
    int d0 = 0, d1 = 0, lit = 0, first = -1;
    for (int p = 0; p < W * H * 4; p++) {
        if (g_px[0][p] != g_px[2][p]) { d0++; if (first < 0) first = p; }
        if (g_px[1][p] != g_px[3][p]) { d1++; if (first < 0) first = p; }
        lit |= g_px[3][p];
    }
    int ok = !d0 && !d1 && (prims <= 2 || lit);
    if (!ok) {
        fprintf(stderr, "%s: %d bytes differ in the count target, %d in the last-primitive target, reference %s", name, d0, d1, lit ? "lit" : "empty");
        if (first >= 0) fprintf(stderr, "; first at (%d, %d): count %02x%02x / %02x%02x, last %02x%02x%02x / %02x%02x%02x", first / 4 % W, first / 4 / W,
                                g_px[0][first & ~3], g_px[0][(first & ~3) + 1], g_px[2][first & ~3], g_px[2][(first & ~3) + 1],
                                g_px[1][first & ~3], g_px[1][(first & ~3) + 1], g_px[1][(first & ~3) + 2], g_px[3][first & ~3], g_px[3][(first & ~3) + 1], g_px[3][(first & ~3) + 2]);
        fprintf(stderr, "\n");
    }
    return ok;
}
static void submit(void) { hv_submit(&g_q, &g_c); g_ib_used = g_ref_used = g_stage_used = 0; }
static uint32_t all_ones(int isz) { return isz == 1 ? 0xffu : isz == 2 ? 0xffffu : 0xffffffffu; }
static void stats(uint64_t s[ST_N]) { hvk_test_index_stats(s); }

// One case: the stream of its form drawn and compared. kind: 0 restart off (the all-ones values are vertices), 1 restart at all ones, 2 at a custom index (13)
static int one(int prim, int isz, int kind, int longer, int gpu_written, const char *tag) {
    uint32_t ri = kind == 2 ? 13u : all_ones(isz ? isz : 4), rv = kind == 2 ? 13u : all_ones(isz ? isz : 4);
    uint32_t n = isz ? stream(g_v, longer, rv, 13u) : (longer ? 1040u : 69u);
    int32_t base = isz == 4 ? 0 : isz ? 7 : 3;   // 4-byte all-ones values plus a base vertex would wrap
    uint32_t inst = longer ? 2 : 1;
    uint32_t prims = draw_pair(prim, isz, kind != 0, ri, g_v, n, base, inst, gpu_written, 0, 1);
    submit();
    char name[200];
    snprintf(name, sizeof name, "%s%s, %s, restart %s, %u %s: the picture of the reference's %u primitives", tag, k_name[prim], isz ? (isz == 1 ? "1-byte indices" : isz == 2 ? "2-byte indices" : "4-byte indices") : "no indices",
             kind == 0 ? "off" : kind == 1 ? "at all ones" : "at 13", n, isz ? "indices" : "vertices", prims);
    int ok = compare(prims, name);
    t(name, ok);
    return ok;
}

// ---------- Draws whose count the GPU holds ----------
// The frontend's records (hle_nvn.c): DrawElementsIndirect (isz 1, 2, 4: count 0, one instance, the index buffer and the record's address) or DrawArraysIndirect (isz 0), and
// record i of an indirect-count multi-draw (cnt: its count's address); the GPU reads {count, instances, first index, base vertex, base instance} or {count, instances, first,
// base instance} there
static void ind_rec(int prim, int isz, const void *ibuf, const void *rec, const void *cnt, int i) {
    if (isz) {
        p_draw_idx d; memset(&d, 0, sizeof d); d.prim = prim; d.itype = isz == 1 ? 0 : isz == 2 ? 1 : 2; d.instances = 1; d.ibuf = (uint64_t)(uintptr_t)ibuf;
        d.ind = (uint64_t)(uintptr_t)rec; d.mdi_cnt = (uint64_t)(uintptr_t)cnt; d.mdi_i = i; hv_emit(&g_c, OP_DRAW_IDX, &d, sizeof d);
    } else { p_draw d; memset(&d, 0, sizeof d); d.prim = prim; d.instances = 1; d.ind = (uint64_t)(uintptr_t)rec; d.mdi_cnt = (uint64_t)(uintptr_t)cnt; d.mdi_i = i; hv_emit(&g_c, OP_DRAW, &d, sizeof d); }
}
// What a dispatch writes earlier in the submission (shaders/ixc_copy.comp): bytes at dst copied from the staging area, dst holding 0xEE bytes when the draw is encoded
static NVNprogram g_copy;
static void gpu_write(uint8_t *dst, const void *src, uint32_t bytes) {
    uint32_t words = (bytes + 3) / 4;
    uint8_t *st = g_stage + g_stage_used; g_stage_used += ((size_t)words * 4 + 4 + 255) & ~(size_t)255;
    if (g_stage_used > (2u << 20)) { fprintf(stderr, "staging full\n"); exit(2); }
    memcpy(st, &words, 4); memset(st + 4, 0, (size_t)words * 4); memcpy(st + 4, src, bytes);
    memset(dst, 0xEE, (size_t)words * 4);
    hv_bind(&g_c, &g_copy, NVN_SHADER_STAGE_COMPUTE_BIT);
    hv_buf(&g_c, OP_BIND_SSBO, 5, 0, st, 4 + (uint64_t)words * 4); hv_buf(&g_c, OP_BIND_SSBO, 5, 1, dst, (uint64_t)words * 4);
    p_dispatch d = {1, 1, 1, 0, 0}; hv_emit(&g_c, OP_DISPATCH, &d, sizeof d);
}
static uint8_t g_img[1u << 20];   // a block as the draw must read it, built on the CPU
static void ref_list(int prim, uint32_t k, uint32_t inst) {   // the oracle's list in g_list (k indices) drawn directly into the bound targets, restart off
    uint32_t cls = (uint32_t)cls_of(prim);
    restart_rec(0, 0);
    uint8_t *rb = ref_take((size_t)k * 4 + 4); memcpy(rb, g_list, (size_t)k * 4);
    hv_draw_idx(&g_c, cls == 1 ? NVN_DRAW_PRIMITIVE_POINTS : cls == 2 ? NVN_DRAW_PRIMITIVE_LINES : NVN_DRAW_PRIMITIVE_TRIANGLES, 2, (int)k, rb, 0, (int)inst);
}
// An indirect draw of a stream into the draw's targets and its reference (the oracle's plain list drawn directly) into the reference's: one block holds the record and, 32 bytes
// on, the stream after IND_FIRST indices of padding (0x5a bytes: the record's first index skips them); written by the CPU before the submission (writer 0) or by a dispatch
// earlier in the submission (1). Base instance 5 (the shaders read no instance). Returns the primitives
enum { IND_FIRST = 3 };
static uint32_t ind_pair(int prim, int isz, int restart, uint32_t ri, const uint32_t *v, uint32_t n, int32_t base, uint32_t inst, int writer) {
    uint32_t k = oracle(prim, isz ? v : NULL, n, isz && restart, ri, base, g_list);
    size_t bytes = 32 + (isz ? (size_t)(IND_FIRST + n) * (size_t)isz : 0);
    uint8_t *blk = ib_take(bytes + 4);
    memset(g_img, 0x5a, bytes);
    uint32_t rec[5] = {n, inst, isz ? (uint32_t)IND_FIRST : (uint32_t)base, isz ? (uint32_t)base : 5u, 5u};
    memcpy(g_img, rec, isz ? 20 : 16);
    if (isz) put_stream(g_img + 32 + IND_FIRST * isz, v, n, isz);
    targets(0);
    restart_rec(restart, ri);
    if (writer) gpu_write(blk, g_img, (uint32_t)bytes); else memcpy(blk, g_img, bytes);
    ind_rec(prim, isz, blk + 32, blk, NULL, 0);
    targets(1);
    ref_list(prim, k, inst);
    return k / (uint32_t)cls_of(prim);
}
// Whether an indirect draw of the form converts (vk_index.c's plan): the primitives Vulkan has not, fans without the device's, 8-bit indices without its support, a restart index
// other than all ones (widened), lists with restart where the device does not restart lists
static int ind_converts(int prim, int isz, int kind, int no_fans, int no_uint8, int no_lr) {
    if (prim == NVN_DRAW_PRIMITIVE_LINE_LOOP || prim == NVN_DRAW_PRIMITIVE_QUADS || prim == NVN_DRAW_PRIMITIVE_QUAD_STRIP) return 1;
    if ((prim == NVN_DRAW_PRIMITIVE_TRIANGLE_FAN || prim == NVN_DRAW_PRIMITIVE_POLYGON) && no_fans) return 1;
    if (!isz) return 0;
    return (isz == 1 && no_uint8) || kind == 2 || (kind && is_list(prim) && no_lr);
}
static int ind_one(int prim, int isz, int kind, int longer, int writer, const char *tag) {
    uint32_t ri = kind == 2 ? 13u : all_ones(isz ? isz : 4), rv = ri;
    uint32_t n = isz ? stream(g_v, longer, rv, 13u) : (longer ? 1040u : 69u);
    int32_t base = isz == 4 ? 0 : isz ? 7 : 3;
    uint32_t inst = longer ? 2 : 1;
    uint32_t prims = ind_pair(prim, isz, kind != 0, ri, g_v, n, base, inst, writer);
    submit();
    char name[220];
    snprintf(name, sizeof name, "%s%s, %s, restart %s, %u %s, %u instance%s: the picture of the reference's %u primitives", tag, k_name[prim],
             isz ? (isz == 1 ? "1-byte indices" : isz == 2 ? "2-byte indices" : "4-byte indices") : "no indices", kind == 0 ? "off" : kind == 1 ? "at all ones" : "at 13", n,
             isz ? "indices" : "vertices", inst, inst > 1 ? "s" : "", prims);
    int ok = compare(prims, name);
    t(name, ok);
    return ok;
}
// Transform feedback (t_vk_transform_feedback's records): xfb5.vert captures 20 bytes a vertex into buffer 0; DrawTransformFeedback is an indirect array draw at the control
// buffer + 16 (hle_nvn.c)
static NVNprogram g_xfb;
static void tf_bind(int i, void *addr, uint64_t size) { p_tf_bind b = {i, 0, (uint64_t)(uintptr_t)addr, size}; hv_emit(&g_c, OP_TF_BIND, &b, sizeof b); }
static void tf_state(int what, void *ctl) { p_tf_state s = {what, 0, (uint64_t)(uintptr_t)ctl}; hv_emit(&g_c, OP_TF_STATE, &s, sizeof s); }   // 0 begin, 1 end
static void rast_discard(int on) { p_rast_discard d = {on}; hv_emit(&g_c, OP_RAST_DISCARD, &d, sizeof d); }
static void capture(void *out, void *ctl, uint32_t n) {   // n vertices captured, the byte offset saved into ctl
    hv_bind(&g_c, &g_xfb, 0x1f); rast_discard(1); tf_bind(0, out, 4096);
    tf_state(0, ctl); hv_draw(&g_c, NVN_DRAW_PRIMITIVE_POINTS, 0, (int)n, 1); tf_state(1, ctl);
    rast_discard(0); tf_bind(0, NULL, 0);
}
static void draw_tf(int prim, void *ctl) { p_draw d; memset(&d, 0, sizeof d); d.prim = prim; d.instances = 1; d.ind = (uint64_t)(uintptr_t)ctl + 16; hv_emit(&g_c, OP_DRAW, &d, sizeof d); }

// ---------- The measurement (argument "bench") ----------
static uint64_t thread_ns(void) {
#if !defined(_WIN32)
    struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#else
    return plat_now_ns();
#endif
}
// d draws of one form in a submission: the conversions' time on the submit thread per draw (the backend's counter: from the readability check to the dispatch recorded, or the
// CPU's list written), the submit thread's CPU time for the whole submission per draw, and the wall time of the submission and its wait per draw
static void bench_one(const char *what, int prim, int isz, int restart, uint32_t ri, uint32_t n, int d, int path_gpu) {
    uint64_t s0[ST_N], s1[ST_N];
    targets(0); restart_rec(restart, ri);
    uint8_t *ib = isz ? ib_take((size_t)n * (size_t)isz + 4) : NULL;
    if (isz) put_stream(ib, g_v, n, isz);
    for (int i = 0; i < d; i++) { if (isz) hv_draw_idx(&g_c, prim, isz == 1 ? 0 : isz == 2 ? 1 : 2, (int)n, ib, 0, 1); else hv_draw(&g_c, prim, 0, (int)n, 1); }
    hv_emit(&g_c, OP_END, NULL, 0);
    stats(s0);
    uint64_t c0 = thread_ns(), w0 = plat_now_ns();
    NVNcommandHandle h = (NVNcommandHandle)(uintptr_t)g_c.base;
    h_nvnQueueSubmitCommands(&g_q, 1, &h);
    uint64_t c1 = thread_ns();
    h_nvnQueueFinish(&g_q);
    uint64_t w1 = plat_now_ns();
    g_c.used = 0; g_ib_used = 0;
    stats(s1);
    uint64_t conv = path_gpu ? s1[ST_GPU] - s0[ST_GPU] : s1[ST_CPU] - s0[ST_CPU], ns = path_gpu ? s1[ST_GPU_NS] - s0[ST_GPU_NS] : s1[ST_CPU_NS] - s0[ST_CPU_NS];
    printf("bench %-58s %s: %3llu conversions, %9.1f us a draw converting on the submit thread, %9.1f us a draw of its CPU time in the submission, %8.3f ms a draw submitted and waited for\n",
           what, path_gpu ? "GPU" : "CPU", (unsigned long long)conv, conv ? ns / 1e3 / (double)conv : 0.0, (c1 - c0) / 1e3 / d, (w1 - w0) / 1e6 / d);
}
static void bench(void) {
    for (int run = 1; run <= 3; run++) {
        printf("bench run %d\n", run);
        for (uint32_t i = 0; i < 40000; i++) g_v[i] = i & 0xffff;   // 10,000 quads of 16-bit indices
        hvk_test_index_opts(0, 0, 0, 1); bench_one("10,000 quads (40,000 2-byte indices)", NVN_DRAW_PRIMITIVE_QUADS, 2, 0, 0, 40000, 20, 0);
        hvk_test_index_opts(0, 0, 0, 0); bench_one("10,000 quads (40,000 2-byte indices)", NVN_DRAW_PRIMITIVE_QUADS, 2, 0, 0, 40000, 20, 1);
        for (uint32_t i = 0; i < 100000; i++) g_v[i] = i % 1000 == 999 ? 13u : i % 50000 + 100;   // a 4-byte fan, restart at 13 every 1,000 indices
        hvk_test_index_opts(1, 0, 0, 1); bench_one("100,000-index fan, restart at 13 (the old path: expanded)", NVN_DRAW_PRIMITIVE_TRIANGLE_FAN, 4, 1, 13, 100000, 20, 0);
        hvk_test_index_opts(0, 0, 0, 0); bench_one("100,000-index fan, restart at 13 (native fan, widened)", NVN_DRAW_PRIMITIVE_TRIANGLE_FAN, 4, 1, 13, 100000, 20, 1);
        hvk_test_index_opts(1, 0, 0, 0); bench_one("100,000-index fan, restart at 13 (fans taken as absent)", NVN_DRAW_PRIMITIVE_TRIANGLE_FAN, 4, 1, 13, 100000, 20, 1);
        for (uint32_t i = 0; i < 24; i++) g_v[i] = i;
        hvk_test_index_opts(0, 0, 0, 1); bench_one("6 quads (24 2-byte indices)", NVN_DRAW_PRIMITIVE_QUADS, 2, 0, 0, 24, 200, 0);
        hvk_test_index_opts(0, 0, 0, 0); bench_one("6 quads (24 2-byte indices)", NVN_DRAW_PRIMITIVE_QUADS, 2, 0, 0, 24, 200, 1);
    }
}

int main(int argc, char **argv) {
    int bench_mode = argc > 1 && !strcmp(argv[1], "bench");
    if (bench_mode) setenv("DM_VK_VALIDATION", "0", 1);
    hv_up("t_vk_index_convert");
    hv_device(0);
    NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&g_q, &qb);
    if (!hv_pool_make(&g_mp, 16u << 20)) { t("memory pool allocated", 0); return hv_finish(); }
    g_c = (hv_cmds){g_mp.mem + (64u << 10), 0, 1u << 20};
    g_stage = g_mp.mem + (2u << 20); g_ib = g_mp.mem + (4u << 20); g_ref = g_mp.mem + (9u << 20);
    const hv_shader sh[2] = {{"ixc_vert", 0, HV_SPV(ixc_vert_spv), NULL, NULL}, {"ixc_frag", 1, HV_SPV(ixc_frag_spv), NULL, NULL}};
    hv_program(&g_prog, &g_mp, 0, sh, 2);
    static const char k_copy[] = "[{\"kind\":\"sbuf\",\"reg\":0,\"name\":\"S\",\"class\":\"ssbo\",\"set\":1,\"binding\":0,\"rw\":false},"
                                 "{\"kind\":\"sbuf\",\"reg\":1,\"name\":\"D\",\"class\":\"ssbo\",\"set\":1,\"binding\":1,\"rw\":true}]";
    const hv_shader shc[1] = {{"ixc_copy", 5, HV_SPV(ixc_copy_comp_spv), k_copy, NULL}}, shx[1] = {{"xfb5", 0, HV_SPV(xfb5_vert_spv), NULL, NULL}};
    hv_program(&g_copy, &g_mp, 1024, shc, 1); hv_program(&g_xfb, &g_mp, 2048, shx, 1);
    for (int i = 0; i < 4; i++) g_t[i] = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, W, H, 1, 1, 0);
    if (bench_mode) { bench(); hv_pool_free(&g_mp); return hv_finish(); }
    uint64_t s0[ST_N], s1[ST_N]; char name[200];
    const hostvk_dev *d = hvk_device();
    printf("note: the device draws triangle fans natively (no VK_KHR_portability_subset here) and has 8-bit indices: %s\n", d && d->x.index_uint8 ? "yes" : "no");

    // 1. Every primitive, index size and restart form, the short and the long stream (with 2 instances); the restart kernel takes the indexed draws with restart of the primitives
    // Vulkan has not (loops, quads, quad strips) and, where the device does not restart lists, of the lists
    int lr = d && d->x.list_restart;
    printf("note: the device restarts lists (VK_EXT_primitive_topology_list_restart): %s\n", lr ? "yes" : "no");
    stats(s0);
    int bad = 0; uint64_t want_restart = 0;
    for (int p = 0; p < NPRIM; p++) for (int isz = 0; isz <= 4; isz = isz ? isz * 2 : 1) for (int kind = 0; kind < 3; kind++) for (int longer = 0; longer < 2; longer++) {
        if (!isz && kind == 2) continue;   // without indices restart is off or on (no index to compare)
        bad += !one(k_prims[p], isz, kind, longer, 0, "");
        int pr = k_prims[p];
        want_restart += isz && kind && (pr == NVN_DRAW_PRIMITIVE_LINE_LOOP || pr == NVN_DRAW_PRIMITIVE_QUADS || pr == NVN_DRAW_PRIMITIVE_QUAD_STRIP || (is_list(pr) && !lr));
    }
    stats(s1);
    snprintf(name, sizeof name, "every conversion of the matrix on the GPU in the prologue: %llu (%llu with the restart kernel, %llu by the rules), %llu on the CPU", (unsigned long long)(s1[ST_GPU] - s0[ST_GPU]),
             (unsigned long long)(s1[ST_GPU_RESTART] - s0[ST_GPU_RESTART]), (unsigned long long)want_restart, (unsigned long long)(s1[ST_CPU] - s0[ST_CPU]));
    t(name, s1[ST_GPU] > s0[ST_GPU] && s1[ST_GPU_PRE] - s0[ST_GPU_PRE] == s1[ST_GPU] - s0[ST_GPU] && s1[ST_CPU] == s0[ST_CPU] && s1[ST_GPU_RESTART] - s0[ST_GPU_RESTART] == want_restart);

    // 1b. Lists with restart converted: the device's restart on lists taken as absent, the same pictures from the restart kernel
    hvk_test_index_opts(-1, -1, 1, -1);
    stats(s0);
    uint64_t lists = 0;
    for (int p = 0; p < NPRIM; p++) for (int isz = 1; isz <= 4; isz *= 2) for (int kind = 1; kind < 3; kind++) for (int longer = 0; longer < 2; longer++) {
        if (!is_list(k_prims[p])) continue;
        bad += !one(k_prims[p], isz, kind, longer, 0, "lists converted: ");
        lists++;
    }
    stats(s1);
    snprintf(name, sizeof name, "lists converted: %llu of %llu draws through the restart kernel, %llu on the CPU", (unsigned long long)(s1[ST_GPU_RESTART] - s0[ST_GPU_RESTART]),
             (unsigned long long)lists, (unsigned long long)(s1[ST_CPU] - s0[ST_CPU]));
    t(name, s1[ST_GPU_RESTART] - s0[ST_GPU_RESTART] == lists && s1[ST_CPU] == s0[ST_CPU]);
    hvk_test_index_opts(-1, -1, 0, -1);

    // 2. A device without triangle fans or 8-bit indices: fans and polygons converted, 8-bit indices widened
    hvk_test_index_opts(1, 1, -1, -1);
    stats(s0);
    for (int p = 0; p < NPRIM; p++) for (int isz = 0; isz <= 4; isz = isz ? isz * 2 : 1) for (int kind = 0; kind < 3; kind++) for (int longer = 0; longer < 2; longer++) {
        int fan = k_prims[p] == NVN_DRAW_PRIMITIVE_TRIANGLE_FAN || k_prims[p] == NVN_DRAW_PRIMITIVE_POLYGON;
        if ((!isz && kind == 2) || !(fan || isz == 1)) continue;
        bad += !one(k_prims[p], isz, kind, longer, 0, "without fans and 8-bit indices: ");
    }
    stats(s1);
    snprintf(name, sizeof name, "without fans and 8-bit indices: %llu conversions on the GPU, %llu on the CPU", (unsigned long long)(s1[ST_GPU] - s0[ST_GPU]), (unsigned long long)(s1[ST_CPU] - s0[ST_CPU]));
    t(name, s1[ST_GPU] > s0[ST_GPU] && s1[ST_CPU] == s0[ST_CPU]);
    hvk_test_index_opts(0, 0, -1, -1);

    // 3. Indices a GPU copy writes earlier in the same submission: converted at the draw's place, before its pass
    stats(s0);
    for (int p = 0; p < NPRIM; p++) for (int isz = 2; isz <= 4; isz *= 2) for (int kind = 0; kind < 3; kind += 2) bad += !one(k_prims[p], isz, kind, 0, 1, "written by a GPU copy: ");
    stats(s1);
    snprintf(name, sizeof name, "indices a GPU copy writes: %llu conversions on the GPU after the copy, %llu in the prologue, %llu on the CPU", (unsigned long long)((s1[ST_GPU] - s0[ST_GPU]) - (s1[ST_GPU_PRE] - s0[ST_GPU_PRE])),
             (unsigned long long)(s1[ST_GPU_PRE] - s0[ST_GPU_PRE]), (unsigned long long)(s1[ST_CPU] - s0[ST_CPU]));
    t(name, s1[ST_GPU] > s0[ST_GPU] && s1[ST_GPU_PRE] == s0[ST_GPU_PRE] && s1[ST_CPU] == s0[ST_CPU]);

    // 4. 0 to 5 indices or vertices, and a stream of restart indices only
    for (int p = 0; p < NPRIM; p++) for (uint32_t n = 0; n <= 5; n++) for (int isz = 0; isz <= 2; isz += 2) {
        for (uint32_t i = 0; i < n; i++) g_v[i] = 40 + 17 * i;
        uint32_t prims = draw_pair(k_prims[p], isz, 1, 13, g_v, n, isz ? 7 : 3, 1, 0, 0, 1);
        submit();
        snprintf(name, sizeof name, "%s, %u %s, restart at 13: the picture of the reference's %u primitives", k_name[k_prims[p]], n, isz ? "2-byte indices" : "vertices", prims);
        int ok = compare(prims, name); t(name, ok); bad += !ok;
    }
    for (int p = 0; p < NPRIM; p++) {
        for (uint32_t i = 0; i < 6; i++) g_v[i] = 0xffff;
        uint32_t prims = draw_pair(k_prims[p], 2, 1, 0xffff, g_v, 6, 7, 1, 0, 0, 1);
        submit();
        snprintf(name, sizeof name, "%s, 6 restart indices only: nothing drawn (the reference's %u primitives)", k_name[k_prims[p]], prims);
        int ok = compare(prims, name); t(name, ok && prims == 0); bad += !ok;
    }

    // 5. The GPU path's limit lowered to 2,400 bytes: a list of exactly that many on the GPU, one past it on the CPU (counted, not red)
    static const struct { int prim, isz, restart, no_fans; uint32_t n; int gpu; } lim[] = {
        {NVN_DRAW_PRIMITIVE_QUADS, 2, 0, 0, 400, 1}, {NVN_DRAW_PRIMITIVE_QUADS, 2, 0, 0, 404, 0},          // 100 quads: 600 indices; 101: 606
        {NVN_DRAW_PRIMITIVE_TRIANGLE_FAN, 4, 1, 1, 202, 1}, {NVN_DRAW_PRIMITIVE_TRIANGLE_FAN, 4, 1, 1, 203, 0},   // a converted fan with restart: at most 600 and 603 indices
        {NVN_DRAW_PRIMITIVE_LINE_LOOP, 0, 0, 0, 300, 1}, {NVN_DRAW_PRIMITIVE_LINE_LOOP, 0, 0, 0, 301, 0},   // a loop without indices: 600 and 602
    };
    for (unsigned i = 0; i < sizeof lim / sizeof *lim; i++) {
        hvk_test_index_opts(lim[i].no_fans, 0, 0, 2400);
        for (uint32_t k = 0; k < lim[i].n; k++) g_v[k] = (k % 97 == 50 && lim[i].restart) ? 13u : 30 + (k * 7) % 200;
        stats(s0);
        uint32_t prims = draw_pair(lim[i].prim, lim[i].isz, lim[i].restart, 13, g_v, lim[i].n, lim[i].isz ? 7 : 3, 1, 0, 0, 1);
        submit();
        stats(s1);
        int gpu = s1[ST_GPU] - s0[ST_GPU] == 1 && s1[ST_CPU] == s0[ST_CPU], cpu = s1[ST_CPU] - s0[ST_CPU] == 1 && s1[ST_CPU_OVER] - s0[ST_CPU_OVER] == 1 && s1[ST_GPU] == s0[ST_GPU];
        snprintf(name, sizeof name, "limit 2,400 bytes: %s of %u %s, at most %llu indices: converted on the %s, the picture of the reference's %u primitives", k_name[lim[i].prim], lim[i].n,
                 lim[i].isz ? "indices" : "vertices", (unsigned long long)(lim[i].gpu ? s1[ST_GPU_OUT] - s0[ST_GPU_OUT] : s1[ST_CPU_OUT] - s0[ST_CPU_OUT]), lim[i].gpu ? "GPU" : "CPU", prims);
        int ok = compare(prims, name) && (lim[i].gpu ? gpu : cpu); t(name, ok); bad += !ok;
    }
    // The CPU path is the GPU's list: the short streams of every form with the limit at 4 bytes (every list past it), and fans and 8-bit indices converted as well
    hvk_test_index_opts(1, 1, 1, 4);
    stats(s0);
    for (int p = 0; p < NPRIM; p++) for (int isz = 0; isz <= 4; isz = isz ? isz * 2 : 1) for (int kind = 0; kind < 3; kind++) {
        if (!isz && kind == 2) continue;
        bad += !one(k_prims[p], isz, kind, 0, 0, "on the CPU: ");
    }
    stats(s1);
    snprintf(name, sizeof name, "on the CPU: %llu conversions on the CPU (all past the limit), %llu on the GPU", (unsigned long long)(s1[ST_CPU] - s0[ST_CPU]), (unsigned long long)(s1[ST_GPU] - s0[ST_GPU]));
    t(name, s1[ST_CPU] > s0[ST_CPU] && s1[ST_CPU_OVER] - s0[ST_CPU_OVER] == s1[ST_CPU] - s0[ST_CPU] && s1[ST_GPU] == s0[ST_GPU]);
    hvk_test_index_opts(0, 0, 0, 0);

    // 6. 400 conversions of six forms in one submission, three submissions in a row: all in the prologue, the ring grown and reused
    // (restart quads, a loop of 1-byte indices, restart quad strips of 4-byte indices, triangles with restart at 13 (widened, or converted where the device does not restart lists), a triangle strip widened for its restart index, a quad
    // strip without indices), all into one pair of targets, then their references into the other
    static const struct { int prim, isz, kind; } mix[] = {{NVN_DRAW_PRIMITIVE_QUADS, 2, 2}, {NVN_DRAW_PRIMITIVE_LINE_LOOP, 1, 0}, {NVN_DRAW_PRIMITIVE_QUAD_STRIP, 4, 2},
                                                         {NVN_DRAW_PRIMITIVE_TRIANGLES, 2, 2}, {NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, 2, 2}, {NVN_DRAW_PRIMITIVE_QUAD_STRIP, 0, 0}};
    static uint32_t at[401];
    for (int sub = 0; sub < 3; sub++) {
        uint32_t prims = 0, used = 0;
        for (int i = 0; i < 400; i++) {   // the streams, kept for the references
            int f = i % 6, isz = mix[f].isz; uint32_t rv = mix[f].kind == 2 ? 13u : all_ones(isz ? isz : 4);
            at[i] = used; used += isz ? stream(g_v + used, 0, rv, 13u) : 24u + (uint32_t)(i % 9);
        }
        at[400] = used;
        stats(s0);
        for (int pass = 0; pass < 2; pass++) {
            targets(pass);
            for (int i = 0; i < 400; i++) {
                int f = i % 6, isz = mix[f].isz; uint32_t rv = mix[f].kind == 2 ? 13u : all_ones(isz ? isz : 4);
                uint32_t k = draw_pair(mix[f].prim, isz, mix[f].kind != 0, rv, g_v + at[i], at[i + 1] - at[i], isz == 4 ? 0 : 5, 1, 0, pass ? -1 : 2, pass ? 2 : -1);
                if (pass) prims += k;
            }
        }
        submit();
        stats(s1);
        snprintf(name, sizeof name, "submission %d: 400 draws of six converted forms (%u primitives), %llu conversions on the GPU in the prologue, %llu on the CPU: the reference's picture", sub + 1,
                 prims, (unsigned long long)(s1[ST_GPU_PRE] - s0[ST_GPU_PRE]), (unsigned long long)(s1[ST_CPU] - s0[ST_CPU]));
        int ok = compare(prims, name) && s1[ST_GPU_PRE] - s0[ST_GPU_PRE] == 400 && s1[ST_GPU] - s0[ST_GPU] == 400 && s1[ST_CPU] == s0[ST_CPU];
        t(name, ok); bad += !ok;
    }
    // 7. Draws whose count the GPU holds (2026-11-02): every primitive, index size and restart form, the short and the long stream, through an indirect record (first index 3,
    // the record's base vertex, instances and base instance) written by the CPU before the submission (converted in the prologue) and by a dispatch earlier in the submission
    // (converted at the draw's place); the native forms go through vkCmdDraw*Indirect as before
    hvk_test_index_opts(0, 0, 0, 0); hvk_test_index_ind(0, 0);
    stats(s0);
    uint64_t want_ind = 0, want_pre = 0;
    for (int p = 0; p < NPRIM; p++) for (int isz = 0; isz <= 4; isz = isz ? isz * 2 : 1) for (int kind = 0; kind < 3; kind++) for (int longer = 0; longer < 2; longer++)
        for (int writer = 0; writer < 2; writer++) {
            if (!isz && kind == 2) continue;
            bad += !ind_one(k_prims[p], isz, kind, longer, writer, writer ? "indirect, written by a dispatch: " : "indirect, written by the CPU: ");
            int c = ind_converts(k_prims[p], isz, kind, 0, 0, !lr);
            want_ind += (uint64_t)c; want_pre += (uint64_t)(c && !writer);
        }
    stats(s1);
    snprintf(name, sizeof name, "indirect matrix: %llu conversions from the record (%llu by the rules), %llu in the prologue (%llu: those the CPU wrote), %llu direct ones, %llu on the CPU",
             (unsigned long long)(s1[ST_IND] - s0[ST_IND]), (unsigned long long)want_ind, (unsigned long long)(s1[ST_IND_PRE] - s0[ST_IND_PRE]), (unsigned long long)want_pre,
             (unsigned long long)(s1[ST_GPU] - s0[ST_GPU]), (unsigned long long)(s1[ST_CPU] - s0[ST_CPU]));
    t(name, s1[ST_IND] - s0[ST_IND] == want_ind && s1[ST_IND_PRE] - s0[ST_IND_PRE] == want_pre && s1[ST_IND_REC] - s0[ST_IND_REC] == want_ind && s1[ST_GPU] == s0[ST_GPU] &&
            s1[ST_CPU] == s0[ST_CPU] && s1[ST_CUT_ARENA] == s0[ST_CUT_ARENA] && s1[ST_CUT_LIMIT] == s0[ST_CUT_LIMIT] && s1[ST_CUT_POOL] == s0[ST_CUT_POOL]);

    // 7b. The device's triangle fans, 8-bit indices and restart on lists taken as absent: fans and polygons, 8-bit indices and lists with restart converted from the record
    hvk_test_index_opts(1, 1, 1, -1);
    stats(s0);
    want_ind = 0;
    for (int p = 0; p < NPRIM; p++) for (int isz = 0; isz <= 4; isz = isz ? isz * 2 : 1) for (int kind = 0; kind < 3; kind++) for (int writer = 0; writer < 2; writer++) {
        int pr = k_prims[p], fan = pr == NVN_DRAW_PRIMITIVE_TRIANGLE_FAN || pr == NVN_DRAW_PRIMITIVE_POLYGON;
        if ((!isz && kind == 2) || !(fan || isz == 1 || (isz && kind && is_list(pr)))) continue;
        bad += !ind_one(pr, isz, kind, 0, writer, writer ? "indirect without fans, 8-bit indices and list restart, by a dispatch: " : "indirect without fans, 8-bit indices and list restart, by the CPU: ");
        want_ind += (uint64_t)ind_converts(pr, isz, kind, 1, 1, 1);
    }
    stats(s1);
    snprintf(name, sizeof name, "indirect without fans, 8-bit indices and list restart: %llu conversions from the record (%llu by the rules)", (unsigned long long)(s1[ST_IND] - s0[ST_IND]),
             (unsigned long long)want_ind);
    t(name, s1[ST_IND] - s0[ST_IND] == want_ind && s1[ST_GPU] == s0[ST_GPU] && s1[ST_CPU] == s0[ST_CPU]);
    hvk_test_index_opts(0, 0, 0, -1);

    // 7c. Indirect-count multi-draws: five records 48 bytes apart over one index buffer (each its own stream and first index) or none (each its own first vertex), the count 3,
    // 0 and 9 (records 3 and 4 hold draws that must not be made under 3; 9 draws all five), written by the CPU and by a dispatch: one dispatch and one draw call a group,
    // each record its own draw (its primitive IDs from 0), against the records drawn one by one
    static const struct { int prim, isz, kind; } md[] = {{NVN_DRAW_PRIMITIVE_QUADS, 2, 2}, {NVN_DRAW_PRIMITIVE_LINE_LOOP, 0, 0}, {NVN_DRAW_PRIMITIVE_QUAD_STRIP, 1, 1}};
    for (unsigned f = 0; f < sizeof md / sizeof *md; f++) for (int writer = 0; writer < 2; writer++) for (int ci = 0; ci < 3; ci++) {
        static const uint32_t counts[3] = {3, 0, 9};
        int pr = md[f].prim, isz = md[f].isz; uint32_t cnt = counts[ci], ri = md[f].kind == 2 ? 13u : all_ones(isz ? isz : 4);
        uint32_t at5[6], used = 0;
        for (int i = 0; i < 5; i++) { at5[i] = used; used += isz ? stream(g_v + used, 0, ri, 13u) : 21u + 6u * (uint32_t)i; }
        at5[5] = used;
        size_t bytes = 5 * 48 + 16 + (isz ? (size_t)used * (size_t)isz : 0);
        uint8_t *blk = ib_take(bytes + 4);
        memset(g_img, 0x5a, bytes);
        for (int i = 0; i < 5; i++) {
            uint32_t rec[5] = {at5[i + 1] - at5[i], 1, at5[i], 7, 0};
            if (!isz) { rec[2] = 3 + 40 * (uint32_t)i; rec[3] = 0; }   // arrays: {count, instances, first, base instance}
            memcpy(g_img + 48 * i, rec, sizeof rec);
        }
        memcpy(g_img + 5 * 48, &cnt, 4);
        if (isz) put_stream(g_img + 5 * 48 + 16, g_v, used, isz);
        stats(s0);
        targets(0);
        restart_rec(md[f].kind != 0, ri);
        if (writer) gpu_write(blk, g_img, (uint32_t)bytes); else memcpy(blk, g_img, bytes);
        for (int i = 0; i < 5; i++) ind_rec(pr, isz, blk + 5 * 48 + 16, blk + 48 * i, blk + 5 * 48, i);
        targets(1);
        uint32_t prims = 0;
        for (uint32_t i = 0; i < 5 && i < cnt; i++) {
            uint32_t k = isz ? oracle(pr, g_v + at5[i], at5[i + 1] - at5[i], md[f].kind != 0, ri, 7, g_list) : oracle(pr, NULL, at5[i + 1] - at5[i], 0, 0, 3 + 40 * (int64_t)i, g_list);
            ref_list(pr, k, 1); prims += k / (uint32_t)cls_of(pr);
        }
        submit();
        stats(s1);
        snprintf(name, sizeof name, "multi-draw of 5 %s records (%s), count %u, written by %s: one conversion of %llu records, the picture of the reference's %u primitives", k_name[pr],
                 isz == 2 ? "2-byte indices, restart at 13" : isz ? "1-byte indices, restart at all ones" : "no indices", cnt, writer ? "a dispatch" : "the CPU",
                 (unsigned long long)(s1[ST_IND_REC] - s0[ST_IND_REC]), prims);
        int ok = compare(prims, name) && s1[ST_IND] - s0[ST_IND] == 1 && s1[ST_IND_MDI] - s0[ST_IND_MDI] == 1 && s1[ST_IND_REC] - s0[ST_IND_REC] == 5 &&
                 s1[ST_IND_PRE] - s0[ST_IND_PRE] == (uint64_t)!writer;
        t(name, ok); bad += !ok;
    }

    // 7d. Records that draw nothing: a count of 0, an instance count of 0 (indexed quads, converted)
    for (int c = 0; c < 2; c++) for (int writer = 0; writer < 2; writer++) {
        for (uint32_t i = 0; i < 40; i++) g_v[i] = 30 + 5 * i;
        uint8_t *blk = ib_take(32 + 44 * 2 + 4);
        memset(g_img, 0x5a, 32 + 44 * 2);
        uint32_t rec[5] = {c == 0 ? 0u : 40u, c == 0 ? 1u : 0u, 0, 7, 0};
        memcpy(g_img, rec, sizeof rec); put_stream(g_img + 32, g_v, 40, 2);
        targets(0); restart_rec(0, 0);
        if (writer) gpu_write(blk, g_img, 32 + 40 * 2); else memcpy(blk, g_img, 32 + 40 * 2);
        ind_rec(NVN_DRAW_PRIMITIVE_QUADS, 2, blk + 32, blk, NULL, 0);
        targets(1);
        submit();
        snprintf(name, sizeof name, "indirect quads with %s, written by %s: nothing drawn", c == 0 ? "a count of 0" : "40 indices and an instance count of 0", writer ? "a dispatch" : "the CPU");
        int ok = compare(0, name); t(name, ok); bad += !ok;
    }

    // 7e. The limits: the arena lowered to 2,400 bytes (600 indices): a draw of 100 quads takes it all (converted in the prologue), the next draw of the submission (converted at its
    // place, after the first) is cut to nothing; a draw of 101 quads alone is cut to 100. A draw's input lowered to 100 indices: 101 quads are cut to 25. Quads without indices past
    // the shared quad list (65,537 quads under rasterizer discard: counted only). A record naming indices past its pool's end: the count stops at the pool's end
    hvk_test_index_ind(2400, -1);
    for (uint32_t k = 0; k < 808; k++) g_v[k] = 30 + (k * 7) % 200;
    stats(s0);
    {
        uint32_t prims = 0, k;
        uint8_t *a = ib_take(32 + 400 * 2 + 4), *b = ib_take(32 + 404 * 2 + 4);
        uint32_t ra[5] = {400, 1, 0, 7, 0}, rb[5] = {404, 1, 0, 7, 0};
        memcpy(g_img, rb, sizeof rb); put_stream(g_img + 32, g_v + 400, 404, 2);
        targets(0); restart_rec(0, 0);
        memcpy(a, ra, sizeof ra); put_stream(a + 32, g_v, 400, 2);
        ind_rec(NVN_DRAW_PRIMITIVE_QUADS, 2, a + 32, a, NULL, 0);
        gpu_write(b, g_img, 32 + 404 * 2);
        ind_rec(NVN_DRAW_PRIMITIVE_QUADS, 2, b + 32, b, NULL, 0);
        targets(1);
        k = oracle(NVN_DRAW_PRIMITIVE_QUADS, g_v, 400, 0, 0, 7, g_list); ref_list(NVN_DRAW_PRIMITIVE_QUADS, k, 1); prims += k / 3;
        submit();
        stats(s1);
        snprintf(name, sizeof name, "arena of 600 indices: 100 quads take it, the next draw of the submission is cut to nothing (%llu cut): the picture of the first's %u primitives",
                 (unsigned long long)(s1[ST_CUT_ARENA] - s0[ST_CUT_ARENA]), prims);
        int ok = compare(prims, name) && s1[ST_CUT_ARENA] - s0[ST_CUT_ARENA] == 1; t(name, ok); bad += !ok;
    }
    for (int writer = 0; writer < 2; writer++) {
        stats(s0);
        uint32_t prims = ind_pair(NVN_DRAW_PRIMITIVE_QUADS, 2, 0, 0, g_v, 404, 7, 1, writer);
        uint32_t k = oracle(NVN_DRAW_PRIMITIVE_QUADS, g_v, 400, 0, 0, 7, g_list);   // the reference: the first 100 quads
        targets(1); ref_list(NVN_DRAW_PRIMITIVE_QUADS, k, 1); prims = k / 3;
        submit();
        stats(s1);
        snprintf(name, sizeof name, "arena of 600 indices, written by %s: 101 quads cut to 100 (%llu cut): the picture of the reference's %u primitives", writer ? "a dispatch" : "the CPU",
                 (unsigned long long)(s1[ST_CUT_ARENA] - s0[ST_CUT_ARENA]), prims);
        int ok = compare(prims, name) && s1[ST_CUT_ARENA] - s0[ST_CUT_ARENA] == 1; t(name, ok); bad += !ok;
    }
    hvk_test_index_ind(0, 100);
    {
        stats(s0);
        uint32_t prims = ind_pair(NVN_DRAW_PRIMITIVE_QUADS, 2, 0, 0, g_v, 404, 7, 1, 0);
        uint32_t k = oracle(NVN_DRAW_PRIMITIVE_QUADS, g_v, 100, 0, 0, 7, g_list);
        targets(1); ref_list(NVN_DRAW_PRIMITIVE_QUADS, k, 1); prims = k / 3;
        submit();
        stats(s1);
        snprintf(name, sizeof name, "a draw's input lowered to 100 indices: 101 quads cut to 25 (%llu cut): the picture of the reference's %u primitives",
                 (unsigned long long)(s1[ST_CUT_LIMIT] - s0[ST_CUT_LIMIT]), prims);
        int ok = compare(prims, name) && s1[ST_CUT_LIMIT] - s0[ST_CUT_LIMIT] == 1; t(name, ok); bad += !ok;
    }
    hvk_test_index_ind(0, 0);
    {
        stats(s0);
        uint8_t *a = ib_take(16 + 4);
        uint32_t ra[4] = {65537u * 4u, 1, 0, 0};
        memcpy(a, ra, sizeof ra);
        targets(0); restart_rec(0, 0); rast_discard(1);
        ind_rec(NVN_DRAW_PRIMITIVE_QUADS, 0, NULL, a, NULL, 0);
        rast_discard(0);
        submit();
        stats(s1);
        snprintf(name, sizeof name, "65,537 quads without indices over the shared list of 65,536: cut there, counted (%llu)", (unsigned long long)(s1[ST_CUT_LIMIT] - s0[ST_CUT_LIMIT]));
        t(name, s1[ST_CUT_LIMIT] - s0[ST_CUT_LIMIT] == 1 && s1[ST_IND] - s0[ST_IND] == 1);
    }
    for (int writer = 0; writer < 2; writer++) {   // the stream ends at the pool's end; the record asks for 40 more indices
        stats(s0);
        uint32_t n = 400;
        uint8_t *end = g_mp.mem + g_mp.size, *ib = end - n * 2, *blk = ib_take(32);
        put_stream(ib, g_v, n, 2);
        uint32_t rec[5] = {n + 40, 1, 0, 7, 0};
        targets(0); restart_rec(0, 0);
        if (writer) gpu_write(blk, rec, sizeof rec); else memcpy(blk, rec, sizeof rec);
        ind_rec(NVN_DRAW_PRIMITIVE_QUADS, 2, ib, blk, NULL, 0);
        targets(1);
        uint32_t k = oracle(NVN_DRAW_PRIMITIVE_QUADS, g_v, n, 0, 0, 7, g_list); ref_list(NVN_DRAW_PRIMITIVE_QUADS, k, 1);
        submit();
        stats(s1);
        snprintf(name, sizeof name, "a record past its index buffer's pool, written by %s: the count stops at the pool's end (%llu counted), the picture of the reference's %u primitives",
                 writer ? "a dispatch" : "the CPU", (unsigned long long)(s1[ST_CUT_POOL] - s0[ST_CUT_POOL]), k / 3);
        int ok = compare(k / 3, name) && s1[ST_CUT_POOL] - s0[ST_CUT_POOL] == 1; t(name, ok); bad += !ok;
    }

    // 7f. DrawTransformFeedback: 69 points captured (20 bytes each), drawn from the capture's count as every primitive, the capture in the same submission (the counter read at the
    // draw's place) and in the one before (in the prologue); the primitives Vulkan has (and fans, polygons on a device with fans) through vkCmdDrawIndirectByteCountEXT, the rest
    // converted from the counter; then fans and polygons with the device's fans taken as absent
    {
        uint8_t *out = g_mp.mem + (15u << 20), *ctl = out + 8192;
        uint64_t want_xfb = 0;
        stats(s0);
        for (int pass = 0; pass < 2; pass++) for (int p = 0; p < NPRIM; p++) for (int same = 0; same < 2; same++) {
            int pr = k_prims[p], fan = pr == NVN_DRAW_PRIMITIVE_TRIANGLE_FAN || pr == NVN_DRAW_PRIMITIVE_POLYGON;
            if (pass && !fan) continue;
            hvk_test_index_opts(pass, -1, -1, -1);
            memset(ctl, 0, 32);
            if (!same) { targets(0); capture(out, ctl, 69); submit(); }
            targets(0);
            if (same) { capture(out, ctl, 69); hv_bind(&g_c, &g_prog, 3); }
            draw_tf(pr, ctl);
            targets(1);
            uint32_t k = oracle(pr, NULL, 69, 0, 0, 0, g_list); ref_list(pr, k, 1);
            submit();
            want_xfb += (uint64_t)ind_converts(pr, 0, 0, pass, 0, 0);
            snprintf(name, sizeof name, "DrawTransformFeedback as %s%s of 69 captured vertices (%u bytes saved), the capture %s: the picture of the reference's %u primitives", k_name[pr],
                     pass ? " (fans taken as absent)" : "", *(uint32_t *)(void *)ctl, same ? "in the same submission" : "in the one before", k / (uint32_t)cls_of(pr));
            int ok = compare(k / (uint32_t)cls_of(pr), name) && *(uint32_t *)(void *)ctl == 69 * 20; t(name, ok); bad += !ok;
        }
        hvk_test_index_opts(0, -1, -1, -1);
        stats(s1);
        snprintf(name, sizeof name, "DrawTransformFeedback: %llu converted from the counter (%llu by the rules)", (unsigned long long)(s1[ST_IND_XFB] - s0[ST_IND_XFB]), (unsigned long long)want_xfb);
        t(name, s1[ST_IND_XFB] - s0[ST_IND_XFB] == want_xfb && s1[ST_IND] - s0[ST_IND] == want_xfb);
    }

    printf("note: %d draws differed from their reference\n", bad);
    hv_pool_free(&g_mp);
    return hv_finish();
}
