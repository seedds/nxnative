// The look-ahead over the rest of a submission (docs/design/vulkan-backend.md 3.3; dm/gfx/hostvk/vk_predict.c, phase P5): after a draw waited for its pipeline, the records still
// to come are walked once over a private copy of the key's state, and the pipelines of the draws ahead that are not ready are queued on the background workers in draw order.
// Eight programs (two vertex and four fragment shaders under keys of their own over two SPIR-V modules) draw blended triangles into an RGBA8 and an RGBA16F target, with a
// polygon mode, a per-instance vertex stream and a blend state changed between draws (the polygon mode and blending baked into the key on levels B and D, the vertex input on D):
// 16 pipelines on levels A and C, 18 on B, 19 on D. On each level the device offers (A and B where its pipeline libraries are usable, C and D forced: hvk_pso_test_level, which drops
// every pipeline before each run), with the optimized relinks off (every level-A and B draw binds its fast link, so the pictures do not depend on when a relink lands):
//   off       the look-ahead off: every pipeline waited for at its first draw; the keys waited for are the expected keys
//   sync      on, the walk returning once the builds it queued are made (a test hook): the keys queued are exactly the keys of the draws ahead (each once), every draw after the
//             first finds its pipeline ready, one draw waits; the same pictures as off
//   held      on, the workers holding the queued builds before they take their key until the submission is done (a test hook delaying the background build): every draw ahead
//             makes its own pipeline as without the look-ahead (none waits behind the queue), the held builds find their keys taken; the same pictures
//   free      on, no hook: every queued key is accounted for at its draw (ready, still being made, or made by the draw); the same pictures
//   calls     two command handles; the first calls a list that calls another, jumps over records the replay skips, and the miss is either before the calls or inside a called list
//             (the walk then returns into the caller through the decoder's return stack before going on to the second handle): the keys queued are those of the draws ahead
//   budget    a budget of 12 records, then of 1 ns: the walk stops, counted, once a submission even though later draws wait again; the same pictures
//   ready     a submission whose draws ahead are partly made already: only the others are queued; the same submission again: no draw waits, no walk
//   list      (on the first level) the device's list (DM_VK_PSO_USER): a P row for every pipeline of a submission, the ones the look-ahead made included, at their first draw
// Under the validation layer, 0 messages. Lavapipe compiles on the CPU: its timing says nothing of a GPU driver's, so the checks count, they do not time
#include "t_vk_harness.h"
#include "vk_pso.h"
#include "shaders/gen/gpl_vary_vert.h"
#include "shaders/gen/gpl_mix_frag.h"

enum { NVS = 2, NFS = 4, NPROG = NVS * NFS, NTRI = 16, W = 64, MAXK = 1024 };
typedef struct { float x, y, z, w; } v4;
static v4 g_pos[3 * NTRI]; static uint8_t g_col[3 * NTRI][4];
static NVNqueue queue; static hv_cmds cmds; static hv_pool mp; static NVNprogram prog[NPROG]; static uint8_t *vb0, *vb1;
static tex_t ta, tb, tc;   // RGBA8, RGBA16F, RG16F
static char name[400], g_user[700];

static void vinput(hv_cmds *c, int divisor1) {   // stream 0 positions, stream 1 colours (per instance with divisor1)
    va_st a[2]; memset(a, 0, sizeof a); a[0].fmt = NVN_FORMAT_RGBA32F; a[0].stream = 0; a[1].fmt = NVN_FORMAT_RGBA8; a[1].stream = 1;
    vs_st s[2] = {{16, 0}, {4, (uint32_t)divisor1}};
    hv_attrs(c, 2, a); hv_streams(c, 2, s); hv_vbuf(c, 0, vb0, sizeof g_pos); hv_vbuf(c, 1, vb1, sizeof g_col);
}
static void color(hv_cmds *c, unsigned blend_en) { color_st cs; memset(&cs, 0, sizeof cs); cs.blend_en = blend_en; cs.logic_op = NVN_LOGIC_OP_COPY; cs.alpha_func = NVN_ALPHA_FUNC_ALWAYS; hv_state(c, 1, &cs, sizeof cs); }
static void poly(hv_cmds *c, int mode) { poly_st p; memset(&p, 0, sizeof p); p.cull = NVN_FACE_NONE; p.front = NVN_FRONT_FACE_CCW; p.mode = (unsigned)mode; hv_state(c, 4, &p, sizeof p); }
static void defaults(hv_cmds *c, const tex_t *t) {   // the targets, cleared, and every state the draws rely on
    const tex_t *rt[1] = {t}; hv_rt(c, 1, rt, NULL);
    p_viewports v; memset(&v, 0, sizeof v); v.n = 1; v.v[0][2] = W; v.v[0][3] = W; v.v[0][5] = 1; hv_emit(c, OP_VIEWPORTS, &v, sizeof v);
    hv_scissor(c, 0, 0, W, W); hv_clear_color(c, 0, 15, 0.1f, 0.2f, 0.3f, 1.0f);
    blend_st b; memset(&b, 0, sizeof b); b.target = 0; b.src_rgb = NVN_BLEND_FUNC_SRC_ALPHA; b.dst_rgb = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA; b.eq_rgb = NVN_BLEND_EQUATION_ADD;
    b.src_a = NVN_BLEND_FUNC_ONE; b.dst_a = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA; b.eq_a = NVN_BLEND_EQUATION_ADD; hv_state(c, 0, &b, sizeof b);
    color(c, 1); chmask_st m = {0xffffffffu}; hv_state(c, 2, &m, sizeof m); poly(c, NVN_POLYGON_MODE_FILL); hv_depth(c, 0, 0, NVN_DEPTH_FUNC_ALWAYS);
    vinput(c, 0);
}
static void draw(hv_cmds *c, int p, int tri) { hv_bind(c, &prog[p], 3); hv_draw(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 3 * (tri % NTRI), 3, 1); }
// The main submission: the first draw is the miss, every other key is ahead of it
static void scene(hv_cmds *c, const tex_t *second) {
    defaults(c, &ta);
    for (int i = 0; i < NPROG; i++) draw(c, i, i);
    draw(c, 0, 8); draw(c, 3, 9);                                                // keys drawn before: the first one (made), one ahead (queued once)
    poly(c, NVN_POLYGON_MODE_LINE); draw(c, 1, 10); poly(c, NVN_POLYGON_MODE_FILL);   // the polygon mode: in the key on level D only
    defaults(c, second);
    for (int i = 0; i < NPROG; i++) draw(c, i, 8 + i);
    vinput(c, 1); hv_bind(c, &prog[2], 3); hv_draw(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 6, 3, 2); vinput(c, 0);   // per-instance colours: the vertex input, in the key on level D only
    color(c, 0); draw(c, 7, 15);                                                  // blending off: in the key on level D only
    hv_submit(&queue, c);
}
typedef struct { uint8_t a[W * W * 4], b[W * W * 8]; } picture;
static int read_picture(picture *p, const tex_t *second) {
    return hv_read(&ta, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, W, W, 4, p->a) && hv_read(second, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, W, W, second == &tb ? 8 : 4, p->b);
}
static int same_picture(const picture *x, const picture *y) { return !memcmp(x->a, y->a, sizeof x->a) && !memcmp(x->b, y->b, sizeof x->b); }

// ---------- Statistics between two points ----------
typedef struct { hvk_pso_stats p; hvk_predict_stats w; } snap;
static void snap_get(snap *s) { hvk_work_drain(HVK_Q_PRED); hvk_pso_stats_get(&s->p); hvk_predict_stats_get(&s->w); }
static uint64_t waits_of(const hvk_pso_stats *s) { uint64_t n = 0; for (int i = 0; i < HVK_W_N; i++) n += s->wait_n[i]; return n; }
typedef struct { uint64_t waits, walks, records, ns, budget, queued, built, found, ready, inflight, made, keys; } delta;
static delta delta_of(const snap *a, const snap *b) {
    return (delta){waits_of(&b->p) - waits_of(&a->p), b->w.walks - a->w.walks, b->w.records - a->w.records, b->w.ns - a->w.ns, b->w.budget_stops - a->w.budget_stops,
                   b->p.pred_queued - a->p.pred_queued, b->p.pred_built - a->p.pred_built, b->p.pred_found - a->p.pred_found, b->p.pred_ready - a->p.pred_ready,
                   b->p.pred_inflight - a->p.pred_inflight, b->p.pred_made - a->p.pred_made, b->w.keys - a->w.keys};
}
// The key digests a run recorded: 0 queued by the look-ahead, 1 waited for by a draw
typedef struct { uint64_t k[MAXK]; int n; } keys;
static void keys_get(keys *k, int which) { k->n = hvk_pso_test_keys_get(which, k->k, MAXK); if (k->n > MAXK) k->n = MAXK; }
static int cmp64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static int has_dup(const keys *k) { keys s = *k; qsort(s.k, (size_t)s.n, 8, cmp64); for (int i = 1; i < s.n; i++) if (s.k[i] == s.k[i - 1]) return 1; return 0; }
static int same_set(const keys *a, const keys *b) {   // a and b hold the same digests (each once)
    if (a->n != b->n || has_dup(a) || has_dup(b)) return 0;
    keys x = *a, y = *b; qsort(x.k, (size_t)x.n, 8, cmp64); qsort(y.k, (size_t)y.n, 8, cmp64);
    return !memcmp(x.k, y.k, (size_t)x.n * 8);
}
static keys ahead_of(const keys *waited) { keys k = *waited; if (k.n) { memmove(k.k, k.k + 1, (size_t)(k.n - 1) * 8); k.n--; } return k; }   // every key but the first draw's

// ---------- Called lists, jumps and a second handle ----------
static uint8_t *g_lists;   // 64 KiB of command memory for the four lists
static void list_end(hv_cmds *c) { hv_emit(c, OP_END, NULL, 0); }
static void call(hv_cmds *c, const hv_cmds *list) { uint64_t t = (uint64_t)(uintptr_t)list->base; hv_emit(c, OP_CALL, &t, 8); }
// variant 0: the miss before the calls (H1: draw 0, CALL X {draw 2, CALL Y {draw 5}, draw 3}, draw 4, JUMP over draw 1, draw 6; H2: draw 7)
// variant 1: the miss inside a called list (H1: CALL X {draw 0, draw 1}, draw 2; H2: draw 3)
static void calls_submit(int variant) {
    hv_cmds x = {g_lists, 0, 16384}, y = {g_lists + 16384, 0, 16384}, h1 = {g_lists + 32768, 0, 16384}, h2 = {g_lists + 49152, 0, 16384};
    if (!variant) {
        draw(&y, 5, 5); list_end(&y);
        draw(&x, 2, 2); call(&x, &y); draw(&x, 3, 3); list_end(&x);
        defaults(&h1, &ta); draw(&h1, 0, 0); call(&h1, &x); draw(&h1, 4, 4);
        uint64_t zero = 0; hv_emit(&h1, OP_JUMP, &zero, 8); size_t at = h1.used - 8;   // its target is patched below
        draw(&h1, 1, 1);                                                             // jumped over: not replayed, and not walked
        uint64_t to = (uint64_t)(uintptr_t)(h1.base + h1.used); memcpy(h1.base + at, &to, 8);
        draw(&h1, 6, 6); list_end(&h1);
        draw(&h2, 7, 7); list_end(&h2);
    } else {
        draw(&x, 0, 0); draw(&x, 1, 1); list_end(&x);
        defaults(&h1, &ta); call(&h1, &x); draw(&h1, 2, 2); list_end(&h1);
        draw(&h2, 3, 3); list_end(&h2);
    }
    NVNcommandHandle h[2] = {(NVNcommandHandle)(uintptr_t)h1.base, (NVNcommandHandle)(uintptr_t)h2.base};
    h_nvnQueueSubmitCommands(&queue, 2, h); h_nvnQueueFinish(&queue);
}

static int user_rows(void) {   // the P rows of the device's list
    FILE *f = fopen(g_user, "r"); char line[NVN_PIPE_ROW_MAX + 8]; int n = 0;
    if (f) { while (fgets(line, sizeof line, f)) n += line[0] == 'P'; fclose(f); }
    return n;
}
static void run_level(char level, int first) {
    picture *off = calloc(1, sizeof *off), *on = calloc(1, sizeof *on);
    if (!off || !on) { t("pictures allocated", 0); free(off); free(on); return; }
    snap a, b; delta d; keys waited_off, queued, waited;
    if (first) {   // the device's list, still empty: one submission of 8 new pipelines, 7 of them made by the look-ahead
        hvk_pso_test_level(level); hvk_predict_test(1, 1, 0, 0);
        hv_cmds *c = &cmds; defaults(c, &tc); for (int i = 0; i < NPROG; i++) draw(c, i, i);
        snap_get(&a); hv_submit(&queue, c); snap_get(&b); d = delta_of(&a, &b);
        int n = user_rows();
        snprintf(name, sizeof name, "level %c, list: the device's list holds a P row for each of the %d pipelines of the submission (%d), the %llu the look-ahead made included", level, NPROG, n,
                 (unsigned long long)d.ready);
        t(name, n == NPROG && d.queued == NPROG - 1 && d.ready == NPROG - 1);
    }
    // off
    hvk_pso_test_level(level); hvk_predict_test(0, 0, 0, 0); hvk_pso_test_keys(1);
    snap_get(&a); scene(&cmds, &tb); snap_get(&b); d = delta_of(&a, &b);
    keys_get(&waited_off, 1);
    snprintf(name, sizeof name, "level %c, off: every pipeline waited for at its first draw (%llu waits, %d keys), no walk", level, (unsigned long long)d.waits, waited_off.n);
    t(name, d.waits == (uint64_t)waited_off.n && waited_off.n == (level == 'D' ? 19 : level == 'B' ? 18 : 16) && d.walks == 0 && d.queued == 0);
    t("off: the targets read back", read_picture(off, &tb));
    keys expect = ahead_of(&waited_off);
    // sync: every draw ahead finds its pipeline ready
    hvk_pso_test_level(level); hvk_predict_test(1, 1, 0, 0); hvk_pso_test_keys(1);
    snap_get(&a); scene(&cmds, &tb); snap_get(&b); d = delta_of(&a, &b);
    keys_get(&queued, 0); keys_get(&waited, 1);
    snprintf(name, sizeof name, "level %c, sync: one walk (%llu records, %.3f ms), %llu keys made, %llu pipelines queued: the %d keys of the draws ahead, each once", level,
             (unsigned long long)d.records, d.ns / 1e6, (unsigned long long)d.keys, (unsigned long long)d.queued, expect.n);
    t(name, d.walks == 1 && same_set(&queued, &expect) && d.queued == (uint64_t)expect.n);
    snprintf(name, sizeof name, "level %c, sync: the draws ahead found their pipeline ready (%llu, off 0) and one draw waited (%llu, off %d)", level, (unsigned long long)d.ready,
             (unsigned long long)d.waits, waited_off.n);
    t(name, d.ready == (uint64_t)expect.n && d.waits == 1 && waited.n == 1 && waited.k[0] == waited_off.k[0] && d.built == (uint64_t)expect.n);
    t("sync: the same pictures as off, bit for bit", read_picture(on, &tb) && same_picture(on, off));
    // the same submission again: no draw waits, so no walk
    snap_get(&a); scene(&cmds, &tb); snap_get(&b); d = delta_of(&a, &b);
    snprintf(name, sizeof name, "level %c: a submission whose pipelines are all made waits 0 times and is not walked (%llu, %llu)", level, (unsigned long long)d.waits, (unsigned long long)d.walks);
    t(name, d.waits == 0 && d.walks == 0);
    // ready: draws ahead partly made (the second target's are, the third's are not; the third's come first)
    hv_cmds *c = &cmds; defaults(c, &tc); for (int i = 0; i < NPROG; i++) draw(c, i, i);
    defaults(c, &tb); for (int i = 0; i < NPROG; i++) draw(c, i, 8 + i);
    snap_get(&a); hv_submit(&queue, c); snap_get(&b); d = delta_of(&a, &b);
    snprintf(name, sizeof name, "level %c: of %d draws ahead, the %d whose pipelines are not made were queued (%llu), the made ones not", level, 2 * NPROG - 1, NPROG - 1, (unsigned long long)d.queued);
    t(name, d.queued == NPROG - 1 && d.waits == 1 && d.walks == 1);
    // held: the queued builds wait before they take their key until the submission is done
    hvk_pso_test_level(level); hvk_predict_test(1, 0, 0, 0); hvk_pso_test_pred_hold(1);
    snap_get(&a); scene(&cmds, &tb); hvk_pso_test_pred_hold(0); snap_get(&b); d = delta_of(&a, &b);
    snprintf(name, sizeof name, "level %c, held: %llu queued, each made by its draw (%llu), none by a worker (%llu); the draws waited as off (%llu)", level, (unsigned long long)d.queued,
             (unsigned long long)d.made, (unsigned long long)d.built, (unsigned long long)d.waits);
    t(name, d.queued == (uint64_t)expect.n && d.made == d.queued && d.built == 0 && d.ready == 0 && d.waits == (uint64_t)waited_off.n);
    t("held: the same pictures as off, bit for bit", read_picture(on, &tb) && same_picture(on, off));
    // free: no hook
    hvk_pso_test_level(level);
    snap_get(&a); scene(&cmds, &tb); snap_get(&b); d = delta_of(&a, &b);
    snprintf(name, sizeof name, "level %c, free: %llu queued, at their draw %llu ready, %llu still being made, %llu made by the draw (%llu draws waited, off %d)", level,
             (unsigned long long)d.queued, (unsigned long long)d.ready, (unsigned long long)d.inflight, (unsigned long long)d.made, (unsigned long long)d.waits, waited_off.n);
    t(name, d.queued == (uint64_t)expect.n && d.ready + d.inflight + d.made == d.queued && d.waits == 1 + d.inflight + d.made);
    t("free: the same pictures as off, bit for bit", read_picture(on, &tb) && same_picture(on, off));
    // calls: the miss before the calls, then inside a called list
    for (int v = 0; v < 2; v++) {
        keys woff; static const int expect_n[2] = {6, 3};
        hvk_pso_test_level(level); hvk_predict_test(0, 0, 0, 0); hvk_pso_test_keys(1); calls_submit(v); keys_get(&woff, 1);
        keys ex = ahead_of(&woff);
        hvk_pso_test_level(level); hvk_predict_test(1, 1, 0, 0); hvk_pso_test_keys(1);
        snap_get(&a); calls_submit(v); snap_get(&b); d = delta_of(&a, &b);
        keys_get(&queued, 0);
        snprintf(name, sizeof name, "level %c, calls (%s): the %d keys ahead, through called lists, a jump, %sthe second handle, queued (%llu) and ready (%llu)", level,
                 v ? "the miss inside a called list" : "the miss before the calls", ex.n, v ? "the return into the caller, " : "", (unsigned long long)d.queued, (unsigned long long)d.ready);
        t(name, ex.n == expect_n[v] && same_set(&queued, &ex) && d.ready == (uint64_t)ex.n && d.waits == 1 && d.walks == 1);
    }
    // budget: 12 records, then 1 ns
    for (int k = 0; k < 2; k++) {
        hvk_pso_test_level(level); hvk_predict_test(1, 1, k ? 0 : 12, k ? 1 : 0);
        snap_get(&a); scene(&cmds, &tb); snap_get(&b); d = delta_of(&a, &b);
        snprintf(name, sizeof name, "level %c, a budget of %s: the walk stopped (%llu records, %llu queued) and walked once though %llu draws waited", level, k ? "1 ns" : "12 records",
                 (unsigned long long)d.records, (unsigned long long)d.queued, (unsigned long long)d.waits);
        t(name, d.budget == 1 && d.walks == 1 && d.queued < (uint64_t)expect.n && (k ? d.records <= 1 : d.records == 12) && d.waits == (uint64_t)waited_off.n - d.queued);
        if (!k) t("budget: the same pictures as off, bit for bit", read_picture(on, &tb) && same_picture(on, off));
    }
    hvk_predict_test(1, 0, 0, 0); hvk_pso_test_keys(0);
    free(off); free(on);
}

int main(void) {
    if (!getenv("DM_NVN_PREDICT")) setenv("DM_NVN_PREDICT", "1", 1);   // on, as a player has it (the runs below switch it with hvk_predict_test)
    const char *tmp = getenv("TMPDIR"); snprintf(g_user, sizeof g_user, "%s/t_vk_predict_user_%d.tsv", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    remove(g_user); setenv("DM_VK_PSO_USER", g_user, 1);
    const hostvk_dev *dev = hv_up("t_vk_predict");
    hv_device(0);
    NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&queue, &qb);
    if (!hv_pool_make(&mp, 1 << 20)) { t("memory pool allocated", 0); return hv_finish(); }
    cmds = (hv_cmds){mp.mem + 65536, 0, 65536}; g_lists = mp.mem + 131072;
    vb0 = mp.mem + 262144; vb1 = vb0 + 4096;
    for (int i = 0; i < NTRI; i++) {   // overlapping triangles over a 4 x 4 grid, blended in draw order
        float cx = -0.75f + 0.5f * (float)(i % 4), cy = -0.75f + 0.5f * (float)(i / 4);
        g_pos[3 * i] = (v4){cx - 0.45f, cy - 0.4f, 0.5f, 1}; g_pos[3 * i + 1] = (v4){cx + 0.5f, cy - 0.3f, 0.5f, 1}; g_pos[3 * i + 2] = (v4){cx - 0.1f, cy + 0.55f, 0.5f, 1};
        for (int v = 0; v < 3; v++) { uint8_t *c = g_col[3 * i + v]; c[0] = (uint8_t)(40 * i + 90 * v); c[1] = (uint8_t)(255 - 13 * i); c[2] = (uint8_t)(70 * v + 5 * i); c[3] = (uint8_t)(120 + 8 * i); }
    }
    memcpy(vb0, g_pos, sizeof g_pos); memcpy(vb1, g_col, sizeof g_col);
    static const char vin[] = "[{\"location\":0,\"type\":\"float\"},{\"location\":1,\"type\":\"float\"}]";
    char vn[NVS][16], fn[NFS][16];
    for (int v = 0; v < NVS; v++) for (int f = 0; f < NFS; f++) {
        snprintf(vn[v], sizeof vn[v], "pr_vs%d", v); snprintf(fn[f], sizeof fn[f], "pr_fs%d", f);
        const hv_shader s[2] = {{vn[v], 0, HV_SPV(gpl_vary_vert_spv), NULL, vin}, {fn[f], 1, HV_SPV(gpl_mix_frag_spv), NULL, NULL}};
        hv_program(&prog[v * NFS + f], &mp, (size_t)(v * NFS + f) * 1024, s, 2);
    }
    ta = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, W, W, 1, 1, 0); tb = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA16F, W, W, 1, 1, 0);
    tc = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RG16F, W, W, 1, 1, 0);
    int gpl = dev->caps.device_level <= 'B' && dev->caps.gpl_fast_link && dev->caps.gpl_indep_interp;
    printf("device level %c, pipeline libraries %s; %d background workers' queue for the look-ahead first\n", dev->caps.device_level, gpl ? "usable: levels A, B, C and D" : "not usable: levels C and D",
           plat_cpu_count() <= 4 ? 1 : 2);
    hvk_pso_test_optimize(0);   // every level-A and B draw binds its fast link (the optimized relinks are bit-exact on lavapipe, t_vk_gpl_identity, but land at times of their own)
    static const char levels[4] = {'A', 'B', 'C', 'D'};
    for (int i = 0, first = 1; i < 4; i++) {
        if (levels[i] <= 'B' && !gpl) { printf("note: level %c: the device has no usable pipeline libraries, not run\n", levels[i]); continue; }
        run_level(levels[i], first); first = 0;
    }
    hvk_predict_stats w; hvk_predict_stats_get(&w); hvk_pso_stats p; hvk_pso_stats_get(&p);
    printf("measured: %llu walks, %llu records in %.3f ms (%.1f ns a record), the longest %.3f ms; %llu pipelines queued, %llu made by the workers\n", (unsigned long long)w.walks,
           (unsigned long long)w.records, w.ns / 1e6, w.records ? (double)w.ns / (double)w.records : 0, w.max_ns / 1e6, (unsigned long long)p.pred_queued, (unsigned long long)p.pred_built);
    char line[2048]; be_pso_line(line, sizeof line); printf("be_pso_line (the log's pipeline line): %s\n", line);
    hvk_pso_test_optimize(1); hvk_pso_test_level('A');
    hv_pool_free(&mp);
    int rc = hv_finish();
    remove(g_user);
    return rc;
}
