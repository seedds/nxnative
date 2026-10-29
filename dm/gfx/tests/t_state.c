// Unit tests of the replay's draw and render-target state (nvn_state.c), CPU only, on every host: state records applied to S and R and the dirty masks they return, the separate
// compute binding (Hyrule Warriors: Age of Calamity), per-stage program binds and their combined programs (Metroid Dread's vertex-only prepass, a ring slot reused under a saved
// pointer: AI: THE SOMNIUM FILES), the defaults of nvn_state_reset, the stages a deferred inline update marks, the snapshot round trip and each queue's own state.
// The backend hooks and hle_nvn.c's nvn_dpool_base are stubs here that record their calls; expected values are written out by hand from NVN's semantics
#include "../nvn/nvn_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail, total;
static void t(const char *name, int ok) {
    total++; fail += !ok;
    printf("%-78s %s\n", name, ok ? "✓" : "✗");
}
#define A NVN_STATE_APPLIED

// ---------- Stubs: hle_nvn.c and the backend's hooks ----------
static dpool_t pool_a, pool_b;
int nvn_dpool_base(const void *pool, int tex) { return pool == &pool_a ? (tex ? 100 : 200) : pool == &pool_b ? (tex ? 300 : 400) : 0; }
static int n_compose; static uint64_t compose_old; static const prog_t *compose_bst[5];
uint64_t nvn_state_hook_compose(uint64_t old_serial, const prog_t *const bst[5]) {
    n_compose++; compose_old = old_serial; memcpy(compose_bst, bst, sizeof compose_bst); return 1000 + (uint64_t)n_compose;
}
static int n_bind, n_bind_bad, n_img_range; static uint32_t bind_op;
void nvn_state_hook_bind(uint32_t op, const p_bind_handles *b) {   // as the Metal backend's bh_fresh: a deferred bind's handles are read again from src, in place
    n_bind++; bind_op = op;
    if (b->defer && b->src && b->n > 0 && b->n <= 32) memcpy((void *)b->h, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n);
}
void nvn_state_hook_bind_bad(const void *rec, int n) { (void)rec; (void)n; n_bind_bad++; }
void nvn_state_hook_img_range(const p_bind_handles *b) { (void)b; n_img_range++; }

static prog_t P[70];   // game programs (only the serial matters to the state)
static uint32_t ap(uint32_t op, const void *a) { return nvn_state_apply(op, a); }
static uint32_t bind_prog(const prog_t *p, int stages) { p_bind_program b = {p, stages}; return ap(OP_BIND_PROGRAM, &b); }

int main(void) {
    for (int i = 0; i < 70; i++) { P[i].magic = MAGIC('P', 'R', 'O', 'G'); P[i].serial = 11 + (uint64_t)i; }
    const prog_t *pa = &P[0], *pb = &P[1], *pc = &P[2];

    // ---------- Defaults ----------
    S.tex_base = 7; R.has_sc = 1; R.sc.x = 5;
    uint32_t d = nvn_state_reset();
    t("reset: pipeline and depth-stencil dirty", d == (NVN_DIRTY_PIPELINE | NVN_DIRTY_DEPTH_STENCIL) && g_state_inited == 1);
    t("reset: blend target i, ONE / ZERO, ADD", S.blend[3].target == 3 && S.blend[3].src_rgb == NVN_BLEND_FUNC_ONE && S.blend[3].dst_rgb == NVN_BLEND_FUNC_ZERO
      && S.blend[3].src_a == NVN_BLEND_FUNC_ONE && S.blend[3].dst_a == NVN_BLEND_FUNC_ZERO && S.blend[3].eq_rgb == NVN_BLEND_EQUATION_ADD && S.blend[3].eq_a == NVN_BLEND_EQUATION_ADD && !S.blend[3].adv);
    t("reset: colour COPY / ALWAYS, all channels written", S.color.logic_op == NVN_LOGIC_OP_COPY && S.color.alpha_func == NVN_ALPHA_FUNC_ALWAYS && !S.color.blend_en && S.chmask.mask == 0xffffffffu);
    t("reset: no culling, CCW front, filled", S.poly.cull == NVN_FACE_NONE && S.poly.front == NVN_FRONT_FACE_CCW && S.poly.mode == NVN_POLYGON_MODE_FILL);
    t("reset: depth write LESS, stencil off ALWAYS / KEEP", !S.ds.dt && S.ds.dw && S.ds.df == NVN_DEPTH_FUNC_LESS && !S.ds.st && S.ds.sf_f == NVN_STENCIL_FUNC_ALWAYS
      && S.ds.sf_b == NVN_STENCIL_FUNC_ALWAYS && S.ds.fail_f == 1 && S.ds.zpass_b == 1);
    t("reset: stencil masks 0xff, depth range 0..1, the rest zero", S.st_vmask[0] == 0xff && S.st_wmask[1] == 0xff && !S.st_ref[0] && S.dr[0] == 0 && S.dr[1] == 1
      && !S.prog && !S.cprog && !S.tex_base && !S.has_vp);
    t("reset leaves R as it is", R.has_sc == 1 && R.sc.x == 5);
    memset(&R, 0, sizeof R);
    uint8_t rec[64] = {0};
    t("not a state record: 0 (draw, clear, end)", !ap(OP_DRAW, rec) && !ap(OP_CLEAR_COLOR, rec) && !ap(OP_END, rec) && !ap(OP_SET_RT, rec));

    // ---------- Scissor, descriptor pools ----------
    p_scissor sc = {1, 2, 30, 40};
    t("scissor: into R, nothing dirty", ap(OP_SCISSOR, &sc) == A && R.has_sc && R.sc.x == 1 && R.sc.w == 30 && R.sc.h == 40);
    p_set_pool sp = {&pool_a};
    t("texture pool: its base, every stage's arguments", ap(OP_SET_TEX_POOL, &sp) == (A | NVN_DIRTY_ARGS_ALL) && S.tex_base == 100 && S.samp_base == 0);
    t("texture pool again: nothing dirty", ap(OP_SET_TEX_POOL, &sp) == A);
    t("sampler pool: its own base", ap(OP_SET_SAMP_POOL, &sp) == (A | NVN_DIRTY_ARGS_ALL) && S.samp_base == 200 && S.tex_base == 100);
    sp.pool = &pool_b;
    t("another texture pool: its base", ap(OP_SET_TEX_POOL, &sp) == (A | NVN_DIRTY_ARGS_ALL) && S.tex_base == 300 && S.samp_base == 200);

    // ---------- Programs: graphics and compute bindings apart ----------
    t("graphics program on vertex | fragment: S.prog, pipeline", bind_prog(pa, 3) == (A | NVN_DIRTY_PIPELINE | NVN_DIRTY_ARGS_ALL) && S.prog == pa && !n_compose);
    t("the same program again: nothing dirty", bind_prog(pa, 3) == A && S.prog == pa);
    t("compute bind: S.cprog, arguments only", bind_prog(pc, 0x20) == (A | NVN_DIRTY_ARGS_ALL) && S.cprog == pc);
    t("compute bind keeps the graphics program (Hyrule Warriors)", S.prog == pa && S.bst[0] == pa && S.bst[1] == pa);
    t("graphics bind keeps the compute program", bind_prog(pb, 3) == (A | NVN_DIRTY_PIPELINE | NVN_DIRTY_ARGS_ALL) && S.prog == pb && S.cprog == pc);
    t("compute unbound by a null compute bind", bind_prog(NULL, 0x20) == (A | NVN_DIRTY_ARGS_ALL) && !S.cprog && S.prog == pb);
    // per-stage binds: vertex from one program, fragment from another make a combined program (the first one of this process: ring slot 0)
    bind_prog(pa, 1);
    const prog_t *ab = S.prog;
    t("vertex A + fragment B: a combined program", ab && ab != pa && ab != pb && ab->magic == MAGIC('P', 'R', 'O', 'G') && ab->serial == 1001 && !ab->nstage);
    t("combined: the backend made it from the bound stages", n_compose == 1 && !compose_old && compose_bst[0] == pa && compose_bst[1] == pb && !compose_bst[2]);
    t("the same combination again: the same object, not made again", bind_prog(pa, 1) == A && S.prog == ab && n_compose == 1);
    t("vertex | fragment of one program again: that program", bind_prog(pa, 3) == (A | NVN_DIRTY_PIPELINE | NVN_DIRTY_ARGS_ALL) && S.prog == pa);
    d = bind_prog(NULL, 2);   // Metroid Dread's depth prepass: BindProgram(null, fragment) leaves the vertex shader
    t("null fragment bind: a vertex-only combined program (Metroid Dread)", d == (A | NVN_DIRTY_PIPELINE | NVN_DIRTY_ARGS_ALL) && S.prog && S.prog != pa
      && n_compose == 2 && compose_bst[0] == pa && !compose_bst[1] && S.bst[0] == pa && !S.bst[1]);
    t("null vertex bind: no program", bind_prog(NULL, 1) == (A | NVN_DIRTY_PIPELINE | NVN_DIRTY_ARGS_ALL) && !S.prog);
    // prog_resolve: a combination taken again from S.bst after a restore
    bind_prog(pa, 1); bind_prog(pb, 2);
    t("A + B again: the combination's ring slot", S.prog == ab && n_compose == 2);
    t("resolve with S.prog current: nothing", nvn_state_prog_resolve() == 0 && S.prog == ab);
    S.prog = pa;
    t("resolve after a stale S.prog: the combination again, pipeline", nvn_state_prog_resolve() == NVN_DIRTY_PIPELINE && S.prog == ab && n_compose == 2);
    nvn_state_snap *snap = malloc(sizeof *snap); nvn_state_save(snap);
    // 62 more combinations (vertex A with fragment P[3..64]) fill the ring's 64 slots; the next one is given slot 0 (A + B)
    for (int i = 0; i < 62; i++) bind_prog(&P[3 + i], 2);
    t("ring full: A + B still cached", (bind_prog(pb, 2), S.prog == ab) && n_compose == 2 + 62);
    bind_prog(&P[65], 2);
    t("65th combination reuses slot 0 and drops its object", S.prog == ab && n_compose == 65 && compose_old == 1001 && compose_bst[1] == &P[65]);
    d = nvn_state_restore(snap);
    t("restore under a reused slot: A + B made again elsewhere (AI: THE SOMNIUM FILES)", d == NVN_DIRTY_PIPELINE && S.prog != ab && S.prog && n_compose == 66
      && compose_old == 1002 && compose_bst[0] == pa && compose_bst[1] == pb && S.bst[0] == pa && S.bst[1] == pb);

    // ---------- Fixed-function state ----------
    p_bind_state bs = {0}; blend_st bl = {.target = 2, .src_rgb = NVN_BLEND_FUNC_SRC_ALPHA, .dst_rgb = NVN_BLEND_FUNC_ONE, .src_a = 2, .dst_a = 1, .eq_rgb = 1, .eq_a = 1};
    memcpy(bs.bytes, &bl, sizeof bl);
    t("blend state: its target's slot, pipeline", ap(OP_BIND_STATE, &bs) == (A | NVN_DIRTY_PIPELINE) && S.blend[2].src_rgb == NVN_BLEND_FUNC_SRC_ALPHA && S.blend[1].src_rgb == NVN_BLEND_FUNC_ONE);
    t("the same blend state again: nothing dirty", ap(OP_BIND_STATE, &bs) == A);
    ds_st ds = {.dt = 1, .dw = 0, .df = NVN_DEPTH_FUNC_ALWAYS}; bs.kind = 5; memset(bs.bytes, 0, sizeof bs.bytes); memcpy(bs.bytes, &ds, sizeof ds);
    t("depth-stencil state: depth-stencil dirty only", ap(OP_BIND_STATE, &bs) == (A | NVN_DIRTY_DEPTH_STENCIL) && S.ds.dt && !S.ds.dw && S.ds.df == NVN_DEPTH_FUNC_ALWAYS);
    color_st co = {.blend_en = 1, .logic_op = NVN_LOGIC_OP_XOR}; bs.kind = 1; memset(bs.bytes, 0, sizeof bs.bytes); memcpy(bs.bytes, &co, sizeof co);
    t("colour state: pipeline", ap(OP_BIND_STATE, &bs) == (A | NVN_DIRTY_PIPELINE) && S.color.blend_en == 1 && S.color.logic_op == NVN_LOGIC_OP_XOR);
    chmask_st cm = {0x0000000f}; bs.kind = 2; memset(bs.bytes, 0, sizeof bs.bytes); memcpy(bs.bytes, &cm, sizeof cm);
    t("channel mask state: pipeline", ap(OP_BIND_STATE, &bs) == (A | NVN_DIRTY_PIPELINE) && S.chmask.mask == 0xf);
    poly_st po = {.cull = NVN_FACE_BACK, .front = NVN_FRONT_FACE_CW, .mode = NVN_POLYGON_MODE_LINE}; bs.kind = 4; memset(bs.bytes, 0, sizeof bs.bytes); memcpy(bs.bytes, &po, sizeof po);
    t("polygon state: pipeline", ap(OP_BIND_STATE, &bs) == (A | NVN_DIRTY_PIPELINE) && S.poly.cull == NVN_FACE_BACK && S.poly.mode == NVN_POLYGON_MODE_LINE);
    ms_st ms = {.enable = 1, .samples = 4, .a2c = 1}; bs.kind = 3; memset(bs.bytes, 0, sizeof bs.bytes); memcpy(bs.bytes, &ms, sizeof ms);
    t("multisample state: pipeline", ap(OP_BIND_STATE, &bs) == (A | NVN_DIRTY_PIPELINE) && S.ms.samples == 4 && S.ms.a2c);
    p_stencil st = {.face = 3, .value_mask = 0x0f, .which = 0};
    t("stencil value mask on both faces: depth-stencil dirty", ap(OP_STENCIL, &st) == (A | NVN_DIRTY_DEPTH_STENCIL) && S.st_vmask[0] == 0x0f && S.st_vmask[1] == 0x0f);
    st = (p_stencil){.face = 2, .mask = 0x3c, .which = 1};
    t("stencil write mask on the back face", ap(OP_STENCIL, &st) == (A | NVN_DIRTY_DEPTH_STENCIL) && S.st_wmask[1] == 0x3c && S.st_wmask[0] == 0xff);
    st = (p_stencil){.face = 1, .ref = 9, .which = 2};
    t("stencil reference: nothing dirty (encoder state)", ap(OP_STENCIL, &st) == A && S.st_ref[0] == 9 && !S.st_ref[1]);
    p_poly_offset pof = {1.5f, 2.0f, 0.25f};
    t("polygon offset", ap(OP_POLY_OFFSET, &pof) == A && S.po.factor == 1.5f && S.po.clamp == 0.25f);
    p_viewports vp = {.first = 1, .n = 2}; for (int i = 0; i < 2; i++) for (int k = 0; k < 6; k++) vp.v[i][k] = (float)(10 * i + k);
    t("viewports 1 and 2: x y w h of each", ap(OP_VIEWPORTS, &vp) == A && S.has_vp && S.vp[1][0] == 0 && S.vp[1][3] == 3 && S.vp[2][2] == 12 && S.vp[0][2] == 0);
    p_depth_range dr = {0.25f, 0.75f}; p_depth_clamp dc = {1};
    t("depth range and depth clamp", ap(OP_DEPTH_RANGE, &dr) == A && ap(OP_DEPTH_CLAMP, &dc) == A && S.dr[0] == 0.25f && S.dr[1] == 0.75f && S.depth_clamp == 1);
    p_vp_swizzle sw = {.first = 0, .n = 1, .sw = {{0, 1, 0, 0}}}; p_blend_color bc = {{0.5f, 0.25f, 1, 0}};
    t("viewport swizzle and blend colour", ap(OP_VP_SWIZZLE, &sw) == A && ap(OP_BLEND_COLOR, &bc) == A && S.vp_sw[0][1] == 1 && S.blend_color[1] == 0.25f);
    p_copy_stride cs = {256, 0}, ci = {4096, 1};
    t("copy row and image strides", ap(OP_COPY_STRIDE, &cs) == A && ap(OP_COPY_STRIDE, &ci) == A && S.copy_stride == 256 && S.copy_img_stride == 4096);
    p_prim_restart pr = {1, 0xffff}; p_rast_discard rd = {1};
    t("primitive restart; rasterizer discard: pipeline", ap(OP_PRIM_RESTART, &pr) == A && S.restart.on && S.restart.index == 0xffff
      && ap(OP_RAST_DISCARD, &rd) == (A | NVN_DIRTY_PIPELINE) && S.rast_discard == 1);
    p_tf_bind tb = {.index = 2, .addr = 0x5000, .size = 64}, tbad = {.index = 4, .addr = 1};
    t("transform feedback buffer 2; index 4 ignored", ap(OP_TF_BIND, &tb) == A && S.tf[2].addr == 0x5000 && ap(OP_TF_BIND, &tbad) == A && !S.tf[3].addr);
    p_tf_state ts = {.what = 0};
    int on0 = (ap(OP_TF_STATE, &ts), S.tf_on); ts.what = 2; int on2 = (ap(OP_TF_STATE, &ts), S.tf_on); ts.what = 3; int on3 = (ap(OP_TF_STATE, &ts), S.tf_on);
    t("transform feedback begin / pause / resume", on0 == 1 && on2 == 0 && on3 == 1);

    // ---------- Vertex input ----------
    struct { int32_t first, n; uint32_t st[16]; } va = {0, 2, {0}}; va_st a0 = {.fmt = 0x2c, .offset = 0, .stream = 0}, a1 = {.fmt = 0x1e, .offset = 12, .stream = 1};
    memcpy(&va.st[0], &a0, 4); memcpy(&va.st[1], &a1, 4);
    t("vertex attributes: pipeline", ap(OP_BIND_VATTR, &va) == (A | NVN_DIRTY_PIPELINE) && S.nattr == 2 && S.attr[1].offset == 12 && S.attr[1].stream == 1);
    t("the same attributes again: nothing dirty", ap(OP_BIND_VATTR, &va) == A);
    p_bind_vstream vs = {0, 1, {{16, 0}}};
    t("vertex streams: pipeline", ap(OP_BIND_VSTREAM, &vs) == (A | NVN_DIRTY_PIPELINE) && S.nstream == 1 && S.stream[0].stride == 16);
    p_bind_vbuf vb = {.first = 1, .n = 2, .r = {{0x10000, 64}, {0x20000, 128}}}; p_bind_vbuf1 v1 = {.index = 5, .size = 32, .addr = 0x30002};
    t("vertex buffers (full and compact records): nothing dirty", ap(OP_BIND_VBUF, &vb) == A && ap(OP_BIND_VBUF1, &v1) == A
      && S.vbuf[1].address == 0x10000 && S.vbuf[2].size == 128 && S.vbuf[5].address == 0x30002 && S.vbuf[5].size == 32 && !S.vbuf[0].address);

    // ---------- Buffers and handles: the stage's arguments ----------
    p_bind_buf ub = {.stage = 1, .index = 3, .addr = 0x1000, .size = 0x100};
    t("uniform buffer stage 1: stage 1's arguments only", ap(OP_BIND_UBO, &ub) == (A | NVN_DIRTY_ARGS(1)) && S.ubo[1][3].addr == 0x1000);
    t("the same uniform buffer again: nothing dirty", ap(OP_BIND_UBO, &ub) == A);
    p_bind_ubo_small us = {.stage = 4, .index = 0, .size = 0x40, .addr = 0x8000};
    t("compact uniform record stage 4: as a full one", ap(OP_BIND_UBO_SMALL, &us) == (A | NVN_DIRTY_ARGS(4)) && S.ubo[4][0].addr == 0x8000 && S.ubo[4][0].size == 0x40 && S.ubo[4][0].stage == 4);
    p_bind_buf u6 = {.stage = 6, .index = 0, .addr = 0x9000, .size = 16};
    t("uniform buffer of stage 6: ignored", ap(OP_BIND_UBO, &u6) == A);
    p_bind_buf sb = {.stage = 5, .index = 2, .addr = 0xa000, .size = 0x200};
    t("storage buffer stage 5: stage 5's arguments", ap(OP_BIND_SSBO, &sb) == (A | NVN_DIRTY_ARGS(5)) && S.ssbo[5][2].size == 0x200);
    p_bind_handles bt = {.stage = 0, .first = 2, .n = 2, .h = {0x00300005, 0x00400006}};
    n_bind = 0;
    t("textures stage 0 slots 2-3: stage 0's arguments, the bind hook first", ap(OP_BIND_TEX, &bt) == (A | NVN_DIRTY_ARGS(0)) && S.tex[0][2] == 0x00300005 && S.tex[0][3] == 0x00400006
      && n_bind == 1 && bind_op == OP_BIND_TEX);
    p_bind_tex1 t1 = {.stage = 1, .slot = 7, .h = 5 | 3u << 20, .tbase = 10, .sbase = 20};
    t("single texture record: IDs plus the pools' bases", ap(OP_BIND_TEX1, &t1) == (A | NVN_DIRTY_ARGS(1)) && S.tex[1][7] == ((5 + 10) | (uint64_t)(3 + 20) << 20) && bind_op == OP_BIND_TEX1);
    uint64_t fresh[2] = {0x00500007, 0x00600008}; p_bind_handles bdf = {.stage = 2, .first = 0, .n = 2, .defer = 1, .src = (uint64_t)(uintptr_t)fresh, .h = {1, 2}};
    t("deferred textures: the handles the hook read again", ap(OP_BIND_TEX, &bdf) == (A | NVN_DIRTY_ARGS(2)) && S.tex[2][0] == 0x00500007 && S.tex[2][1] == 0x00600008);
    p_bind_handles bbad = {.stage = 3, .first = 0, .n = 33, .h = {9}};
    t("texture record with count 33: reported, skipped", ap(OP_BIND_TEX, &bbad) == A && n_bind_bad == 1 && !S.tex[3][0]);
    p_bind_handles bsm = {.stage = 2, .first = 126, .n = 2, .h = {0x00100000, 0x00200000}};
    t("samplers stage 2 slots 126-127", ap(OP_BIND_SAMP, &bsm) == (A | NVN_DIRTY_ARGS(2)) && S.samp[2][127] == 0x00200000 && bind_op == OP_BIND_SAMP);
    bsm.first = 127;
    t("samplers past slot 127: ignored", ap(OP_BIND_SAMP, &bsm) == A);
    p_bind_handles bim = {.stage = 3, .first = 30, .n = 2, .h = {0x11, 0x12}};
    t("images stage 3 slots 30-31", ap(OP_BIND_IMG, &bim) == (A | NVN_DIRTY_ARGS(3)) && S.img[3][31] == 0x12 && !n_img_range);
    bim.first = 31;
    t("images past slot 31: reported, skipped", ap(OP_BIND_IMG, &bim) == A && n_img_range == 1 && S.img[3][31] == 0x12);

    // ---------- The stages a deferred inline update marks ----------
    t("update inside stage 1's uniform block: stage 1 only", nvn_state_ubo_dirty(0x1080, 0x1090) == NVN_DIRTY_ARGS(1));
    t("update over stage 4's block: stage 4 only", nvn_state_ubo_dirty(0x8000, 0x8001) == NVN_DIRTY_ARGS(4));
    t("update just past stage 1's block: none", nvn_state_ubo_dirty(0x1100, 0x1200) == 0 && nvn_state_ubo_dirty(0x0f00, 0x1000) == 0);
    t("update over both blocks: stages 1 and 4", nvn_state_ubo_dirty(0, 0x10000) == (NVN_DIRTY_ARGS(1) | NVN_DIRTY_ARGS(4)));
    t("storage buffers do not count", nvn_state_ubo_dirty(0xa000, 0xa100) == 0);

    // ---------- Snapshot round trip ----------
    nvn_state_save(snap);
    nvn_draw_state *s0 = malloc(sizeof *s0); *s0 = S; nvn_rt_state r0 = R;
    S.tex_base = 9999; S.tex[5][100] = 77; S.ds.df = NVN_DEPTH_FUNC_LESS; R.sc.x = 4242; R.cc[1].green = 0.5;
    nvn_state_restore(snap);
    t("save, change, restore: S and R as saved", !memcmp(&S, s0, sizeof S) && !memcmp(&R, &r0, sizeof R));
    free(s0);

    // ---------- Each queue's own state ----------
    d = nvn_state_queue_activate(1);
    t("first queue: every dirty bit, defaults, R cleared", d == (NVN_DIRTY_PIPELINE | NVN_DIRTY_DEPTH_STENCIL | NVN_DIRTY_ARGS_ALL) && !S.tex_base && !S.prog && !R.has_sc && S.dr[1] == 1);
    S.tex_base = 123; R.has_sc = 1; R.sc.x = 77; bind_prog(pa, 3);
    t("another queue: independent defaults", nvn_state_queue_activate(2) != 0 && !S.tex_base && !R.has_sc && !S.bst[0] && !S.prog);
    S.tex_base = 456;
    t("the active queue again: nothing", nvn_state_queue_activate(2) == 0 && S.tex_base == 456);
    d = nvn_state_queue_activate(1);
    t("back to the first queue: its state restored", d == (NVN_DIRTY_PIPELINE | NVN_DIRTY_DEPTH_STENCIL | NVN_DIRTY_ARGS_ALL) && S.tex_base == 123 && R.has_sc && R.sc.x == 77 && S.prog == pa);
    t("forgetting an inactive queue: nothing dirty, state kept", nvn_state_queue_forget(2) == 0 && S.tex_base == 123);
    t("forgetting an unknown queue: nothing", nvn_state_queue_forget(99) == 0);
    d = nvn_state_queue_forget(1);
    t("forgetting the active queue: defaults, every dirty bit", d == (NVN_DIRTY_PIPELINE | NVN_DIRTY_DEPTH_STENCIL | NVN_DIRTY_ARGS_ALL) && !S.tex_base && !S.prog && !S.bst[0] && !R.has_sc);
    nvn_state_queue_activate(2);
    t("a forgotten queue starts again from the defaults", !S.tex_base && !R.has_sc);
    free(snap);
    printf("== t_state: %d checks, %d failed ==\n", total, fail);
    return fail != 0;
}
